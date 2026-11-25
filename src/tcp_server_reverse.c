#include "file_pool.h"
#include "http.h"
#include "proxy_response.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    LISTEN_PORT = 9200,
    DEFAULT_STATIC_PORT = 9000,
    DEFAULT_CGI_PORT = 9001,
    MAX_BACKENDS = 16,
    MAX_ROUTES = 16,
    MAX_CLIENTS = 10000,
    ACCEPT_BATCH_SIZE = 64,
    MAX_EVENTS = MAX_CLIENTS * 3 + MAX_BACKENDS + 6,
    BUFFER_SIZE = 4096,
    ORIGIN_AUTH_HEADER_CAPACITY = sizeof(HTTP_ORIGIN_TOKEN_HEADER ": \r\n") + HTTP_ORIGIN_TOKEN_LENGTH,
    CLIENT_TIMEOUT_MS = 5000,
    HEALTH_CHECK_TIMEOUT_MS = 1000,
    HEALTH_INTERVAL_TIMER = MAX_CLIENTS,
    HEALTH_DEADLINE_TIMER
};

struct health_probe {
    int fd;
    short filter;
    bool connecting;
    char request[128 + ORIGIN_AUTH_HEADER_CAPACITY];
    size_t request_length;
    size_t request_sent;
    char headers[HTTP_MAX_HEADERS + 1];
    size_t header_length;
};

struct backend_target {
    int port;
    bool healthy;
    struct health_probe probe;
};

struct backend_group {
    size_t indices[MAX_BACKENDS];
    size_t count;
    size_t next;
};

struct proxy_route {
    const char *path;
    size_t path_length;
    struct backend_group backends;
    const struct backend_target *standby;
};

struct backend_pool {
    struct backend_target targets[MAX_BACKENDS];
    size_t count;
    struct backend_group fallback;
    struct proxy_route routes[MAX_ROUTES];
    size_t route_count;
    int health_interval_ms;
    char origin_auth_header[ORIGIN_AUTH_HEADER_CAPACITY];
};

struct backend_connection {
    int fd;
    int port;
    short filter;
    bool connecting;
    bool response_started;
    char request_headers[HTTP_MAX_HEADERS + 128 + ORIGIN_AUTH_HEADER_CAPACITY];
    size_t request_header_length;
    size_t request_sent;
    char raw_headers[HTTP_MAX_HEADERS + 1];
    size_t raw_header_length;
    size_t header_sent;
    struct proxy_response response;
};

struct http_connection {
    int fd;
    int slot;
    uint64_t connection_id;
    short filter;
    struct backend_pool *origins;
    struct backend_connection *backend;
    struct file_pool *file_pool;
    struct file_job *file;
    size_t file_sent;
    bool file_pending;
    char input[BUFFER_SIZE];
    size_t input_length;
    bool timer_armed;
    char headers[HTTP_MAX_HEADERS + 1];
    char parsed_headers[HTTP_MAX_HEADERS + 1];
    size_t header_length;
    bool headers_complete;
    struct http_request request;
    size_t body_received;
    char response[HTTP_RESPONSE_SIZE];
    size_t response_length;
    size_t response_sent;
    bool interim_response;
    bool keep_alive;
};

static bool set_backend_filter(int queue, struct http_connection *client, short next);
static bool arm_timeout(int queue, struct http_connection *client);

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    // The event loop receives the same signal through EVFILT_SIGNAL.
}

static int configure_socket(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int connect_backend(int port, bool *connecting)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("open backend socket");
        return -1;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (configure_socket(fd) == -1 || inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    *connecting = false;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        if (errno != EINPROGRESS && errno != EINTR) {
            fprintf(stderr, "connect to backend port %d: %s\n", port, strerror(errno));
            close(fd);
            return -1;
        }
        *connecting = true;
    }
    return fd;
}

static bool connection_ready(int fd)
{
    int error;
    socklen_t length = sizeof(error);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
}

// Return 0 while more bytes are needed, or an HTTP status when ready to reply.
static int read_request_bytes(struct http_connection *state, const char *data,
                              size_t length, size_t *consumed)
{
    const char *start = data;
    while (!state->headers_complete && length > 0) {
        char byte = *data++;
        length--;
        size_t count = state->header_length;
        char previous = count == 0 ? '\0' : state->headers[count - 1];
        if (byte == '\0' || (byte == '\n' && previous != '\r') ||
            (previous == '\r' && byte != '\n')) {
            return 400;
        }

        state->headers[count++] = byte;
        state->header_length = count;
        if (count >= 4 && memcmp(state->headers + count - 4, "\r\n\r\n", 4) == 0) {
            state->headers[count] = '\0';
            memcpy(state->parsed_headers, state->headers, count + 1);
            int status = http_parse_headers(state->parsed_headers, &state->request, NULL);
            if (status != 200) {
                return status;
            }
            if (state->request.content_length > 0) {
                state->request.body = malloc(state->request.content_length);
                if (state->request.body == NULL) {
                    return 500;
                }
            }
            state->headers_complete = true;
        } else if (count == HTTP_MAX_HEADERS) {
            return 431;
        }
    }

    *consumed = (size_t)(data - start);
    if (!state->headers_complete) {
        return 0;
    }
    size_t remaining = state->request.content_length - state->body_received;
    if (length > remaining) {
        length = remaining;
    }
    if (length > 0) {
        memcpy(state->request.body + state->body_received, data, length);
        state->body_received += length;
    }
    *consumed += length;

    if (state->body_received == state->request.content_length) {
        return 200;
    }
    if (state->request.expect_continue) {
        state->request.expect_continue = false;
        return 100;
    }
    return 0;
}

static struct proxy_route *find_route(struct backend_pool *pool, const char *path, size_t length)
{
    for (size_t i = 0; i < pool->route_count; i++) {
        struct proxy_route *route = &pool->routes[i];
        if (route->path_length == length && memcmp(route->path, path, length) == 0) {
            return route;
        }
    }
    return NULL;
}

static int next_backend(struct backend_pool *pool, const char *uri)
{
    struct proxy_route *route = find_route(pool, uri, strcspn(uri, "?"));
    struct backend_group *group = route == NULL ? &pool->fallback : &route->backends;
    for (size_t i = 0; i < group->count; i++) {
        const struct backend_target *target = &pool->targets[group->indices[group->next]];
        group->next = (group->next + 1) % group->count;
        if (pool->health_interval_ms == 0 || target->healthy) {
            return target->port;
        }
    }
    if (route != NULL && route->standby != NULL && route->standby->healthy) {
        return route->standby->port;
    }
    return 0;
}

static bool replaced_request_header(const char *name, size_t length)
{
    static const char *const names[] = {
        "Host", "Connection", "Content-Length", "Expect", "Keep-Alive", "Proxy-Connection",
        "Proxy-Authorization", "TE", "Trailer", "Transfer-Encoding", "Upgrade",
        HTTP_ORIGIN_TOKEN_HEADER
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == length && strncasecmp(name, names[i], length) == 0) {
            return true;
        }
    }
    return false;
}

static bool build_backend_request(const struct http_connection *client,
                                  struct backend_connection *backend)
{
    const struct http_request *request = &client->request;
    char *output = backend->request_headers;
    size_t capacity = sizeof(backend->request_headers);
    int length = snprintf(output, capacity,
                          "%s %s %s\r\nHost: 127.0.0.1:%d\r\nContent-Length: %zu\r\nConnection: close\r\n%s",
                          request->method, request->uri, request->version, backend->port,
                          request->content_length, client->origins->origin_auth_header);
    if (length < 0 || (size_t)length >= capacity) {
        return false;
    }

    // http_parse_headers already validated the request. Retain its raw end-to-end fields.
    const char *request_line_end = strstr(client->headers, "\r\n");
    if (request_line_end == NULL) {
        return false;
    }
    const char *fields = request_line_end + 2;
    char options[HTTP_MAX_HEADERS + 1] = "";
    size_t options_length = 0;
    for (const char *line = fields; *line != '\r';) {
        const char *end = strstr(line, "\r\n");
        const char *colon = strchr(line, ':');
        if (end == NULL || colon == NULL || colon >= end) {
            return false;
        }
        size_t name_length = (size_t)(colon - line);
        if (name_length == sizeof("Connection") - 1 &&
            strncasecmp(line, "Connection", name_length) == 0) {
            const char *value = colon + 1;
            size_t value_length = (size_t)(end - value);
            memcpy(options + options_length, value, value_length);
            options_length += value_length;
            options[options_length++] = ',';
        }
        line = end + 2;
    }
    options[options_length] = '\0';

    size_t used = (size_t)length;
    for (const char *line = fields; *line != '\r';) {
        const char *end = strstr(line, "\r\n");
        const char *colon = strchr(line, ':');
        if (end == NULL || colon == NULL || colon >= end) {
            return false;
        }
        const char *next = end + 2;
        size_t name_length = (size_t)(colon - line);
        if (!replaced_request_header(line, name_length) &&
            !proxy_connection_option(options, line, name_length)) {
            size_t field_length = (size_t)(next - line);
            if (field_length > capacity - used) {
                return false;
            }
            memcpy(output + used, line, field_length);
            used += field_length;
        }
        line = next;
    }
    if (capacity - used < 2) {
        return false;
    }
    memcpy(output + used, "\r\n", 2);
    backend->request_header_length = used + 2;
    return true;
}

static bool prepare_response(struct http_connection *state, int length, int status)
{
    if (length < 0 || (size_t)length >= sizeof(state->response)) {
        return false;
    }
    state->response_length = (size_t)length;
    state->response_sent = 0;
    state->interim_response = status == 100;
    return true;
}

static bool queue_response(struct http_connection *state, int status)
{
    int length = http_write_response(state->response, sizeof(state->response),
                                     state->request.version, status, "", "", state->keep_alive);
    return prepare_response(state, length, status);
}

static bool is_offloaded_asset(const char *uri)
{
    static const char logo[] = "/static/logo.png";
    static const char css[] = "/static/style.css";
    size_t length = strcspn(uri, "?");
    return (length == sizeof(logo) - 1 && memcmp(uri, logo, length) == 0) ||
           (length == sizeof(css) - 1 && memcmp(uri, css, length) == 0);
}

static void close_backend(struct http_connection *client)
{
    if (client->backend != NULL) {
        close(client->backend->fd);
        free(client->backend);
        client->backend = NULL;
    }
}

static bool start_backend(int queue, struct http_connection *client)
{
    int port = next_backend(client->origins, client->request.uri);
    if (port == 0) {
        return queue_response(client, 503);
    }
    struct backend_connection *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return queue_response(client, 500);
    }
    backend->port = port;
    if (!build_backend_request(client, backend)) {
        free(backend);
        return queue_response(client, 500);
    }

    backend->fd = connect_backend(backend->port, &backend->connecting);
    if (backend->fd == -1) {
        free(backend);
        return queue_response(client, 502);
    }
    client->backend = backend;

    if (!set_backend_filter(queue, client, EVFILT_WRITE) || !arm_timeout(queue, client)) {
        close_backend(client);
        return queue_response(client, 500);
    }
    return true;
}

static void reset_request(struct http_connection *state)
{
    free(state->request.body);
    state->request = (struct http_request){0};
    if (state->file != NULL) {
        file_pool_release(state->file_pool, state->file);
        state->file = NULL;
    }
    state->file_sent = 0;
    state->file_pending = false;
    state->header_length = 0;
    state->headers_complete = false;
    state->body_received = 0;
    state->response_length = 0;
    state->response_sent = 0;
    state->interim_response = false;
    state->keep_alive = false;
}

static bool process_client_input(int queue, struct http_connection *state)
{
    size_t consumed = 0;
    int status = read_request_bytes(state, state->input, state->input_length, &consumed);
    state->input_length -= consumed;
    if (state->input_length > 0 && consumed > 0) {
        memmove(state->input, state->input + consumed, state->input_length);
    }
    if (status == 0) {
        return true;
    }
    if (status == 200) {
        if (state->request.method == NULL || state->request.uri == NULL ||
            state->request.version == NULL) {
            return queue_response(state, 500);
        }
        if (state->request.uri[0] != '/') {
            return queue_response(state, 400);
        }
        state->keep_alive = !state->request.connection_close &&
                            (strcmp(state->request.version, "HTTP/1.1") == 0 ||
                             state->request.connection_keep_alive);
        if (state->file_pool != NULL && strcmp(state->request.method, "GET") == 0 &&
            state->request.content_length == 0 && is_offloaded_asset(state->request.uri)) {
            const char *path = state->request.uri + sizeof("/static/") - 1;
            int file_status = file_pool_submit_open(state->file_pool, state->slot,
                                                    state->connection_id, path);
            state->file_pending = file_status == 0;
            return state->file_pending || queue_response(state, file_status);
        }
        return start_backend(queue, state);
    }
    return queue_response(state, status);
}

static void cancel_timeout(int queue, struct http_connection *state)
{
    if (!state->timer_armed) {
        return;
    }
    struct kevent change;
    EV_SET(&change, (uintptr_t)state->slot, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1 && errno != ENOENT) {
        perror("kevent: remove timeout");
    }
    state->timer_armed = false;
}

static bool arm_timeout(int queue, struct http_connection *state)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)state->slot, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           0, CLIENT_TIMEOUT_MS, state);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1) {
        perror("kevent: watch timeout");
        return false;
    }
    state->timer_armed = true;
    return true;
}

static bool finish_response(int queue, struct http_connection *state)
{
    close_backend(state);
    if (!state->keep_alive) {
        return false;
    }
    reset_request(state);
    if (state->input_length > 0) {
        return arm_timeout(queue, state) && process_client_input(queue, state);
    }
    return arm_timeout(queue, state);
}

static bool can_retry_socket_io(bool hangup)
{
    return errno == EINTR ||
           ((errno == EAGAIN || errno == EWOULDBLOCK) && !hangup);
}

static bool backend_response_pending(const struct http_connection *client)
{
    return client->backend->header_sent < client->backend->response.header_length ||
           client->response_sent < client->response_length;
}

static bool send_backend_response(int queue, struct http_connection *client, bool hangup)
{
    struct backend_connection *backend = client->backend;
    bool sending_headers = backend->header_sent < backend->response.header_length;
    const char *data = sending_headers ? backend->response.headers : client->response;
    size_t length = sending_headers ? backend->response.header_length : client->response_length;
    size_t *offset = sending_headers ? &backend->header_sent : &client->response_sent;
    ssize_t sent = send(client->fd, data + *offset, length - *offset, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    backend->response_started = true;
    *offset += (size_t)sent;
    if (backend_response_pending(client)) {
        return arm_timeout(queue, client);
    }

    client->response_length = 0;
    client->response_sent = 0;
    if (backend->response.has_length && backend->response.remaining == 0) {
        return finish_response(queue, client);
    }
    return set_backend_filter(queue, client, EVFILT_READ) && arm_timeout(queue, client);
}

static bool send_file_chunk(int queue, struct http_connection *state, bool hangup)
{
    struct file_job *job = state->file;
    ssize_t sent = send(state->fd, job->data + state->file_sent,
                        job->length - state->file_sent, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    state->file_sent += (size_t)sent;
    job->remaining -= (off_t)sent;
    if (job->remaining == 0) {
        return finish_response(queue, state);
    }
    if (state->file_sent == job->length) {
        state->file = NULL;
        state->file_pending = true;
        file_pool_submit_read(state->file_pool, job);
    }
    return arm_timeout(queue, state);
}

static bool handle_client(int queue, struct http_connection *state, bool hangup)
{
    if (state->backend != NULL) {
        return send_backend_response(queue, state, hangup);
    }
    if (state->response_length == 0) {
        ssize_t received = recv(state->fd, state->input + state->input_length,
                                sizeof(state->input) - state->input_length, 0);
        if (received > 0) {
            state->input_length += (size_t)received;
            return arm_timeout(queue, state) && process_client_input(queue, state);
        }
        if (received == 0) {
            return state->header_length > 0 && queue_response(state, 400);
        }
        return can_retry_socket_io(hangup);
    }

    if (state->file != NULL && state->response_sent == state->response_length) {
        return send_file_chunk(queue, state, hangup);
    }

    ssize_t sent = send(state->fd, state->response + state->response_sent,
                        state->response_length - state->response_sent, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    state->response_sent += (size_t)sent;
    if (state->response_sent < state->response_length) {
        return arm_timeout(queue, state);
    }
    if (state->interim_response) {
        // After 100 Continue, resume reading this request's body.
        state->response_length = 0;
        state->response_sent = 0;
        state->interim_response = false;
        return arm_timeout(queue, state);
    }
    if (state->file != NULL && state->file->remaining > 0) {
        return arm_timeout(queue, state);
    }
    return finish_response(queue, state);
}

static void close_client(int queue, struct http_connection *client)
{
    cancel_timeout(queue, client);
    close_backend(client);
    reset_request(client);
    close(client->fd);
    client->fd = -1;
}

enum { ACCEPT_RETRY = -1, ACCEPT_FAILED = -2 };

static int accept_client(int server_fd, struct http_connection clients[],
                         struct backend_pool *origins, struct file_pool *file_pool)
{
    int fd = accept(server_fd, NULL, NULL);
    if (fd == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == ECONNABORTED) {
            return ACCEPT_RETRY;
        }
        perror("accept");
        return ACCEPT_FAILED;
    }

    int slot = 0;
    while (slot < MAX_CLIENTS && clients[slot].fd != -1) {
        slot++;
    }
    if (slot == MAX_CLIENTS) {
        close(fd);
        return ACCEPT_RETRY;
    }
    if (configure_socket(fd) == -1) {
        perror("fcntl");
        close(fd);
        return ACCEPT_RETRY;
    }

    clients[slot] = (struct http_connection){
        .fd = fd,
        .slot = slot,
        .connection_id = clients[slot].connection_id + 1,
        .filter = EVFILT_READ,
        .origins = origins,
        .file_pool = file_pool,
    };
    return slot;
}

static int watch_read(int queue, int fd, struct http_connection *client)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0, client);
    return kevent(queue, &change, 1, NULL, 0, NULL);
}

static bool set_socket_filter(int queue, void *event_data, int fd,
                              short *filter, short next_filter)
{
    if (*filter == next_filter) {
        return true;
    }
    struct kevent changes[2];
    int count = 0;
    if (*filter != 0) {
        EV_SET(&changes[count], (uintptr_t)fd, *filter, EV_DELETE, 0, 0, NULL);
        count++;
    }
    if (next_filter != 0) {
        EV_SET(&changes[count], (uintptr_t)fd, next_filter, EV_ADD, 0, 0, event_data);
        count++;
    }
    if (kevent(queue, changes, count, NULL, 0, NULL) == -1) {
        perror("kevent: change socket filter");
        return false;
    }
    *filter = next_filter;
    return true;
}

static bool set_client_filter(int queue, struct http_connection *client, short next)
{
    return set_socket_filter(queue, client, client->fd, &client->filter, next);
}

static bool set_backend_filter(int queue, struct http_connection *client, short next)
{
    struct backend_connection *backend = client->backend;
    return set_socket_filter(queue, client, backend->fd, &backend->filter, next);
}

static void finish_health_probe(struct backend_target *target, bool healthy)
{
    close(target->probe.fd);
    target->probe.fd = -1;
    target->healthy = healthy;
}

static void expire_health_probes(struct backend_pool *pool)
{
    for (size_t i = 0; i < pool->count; i++) {
        if (pool->targets[i].probe.fd != -1) {
            finish_health_probe(&pool->targets[i], false);
        }
    }
}

static bool watch_health_timer(int queue, uintptr_t ident, int milliseconds, unsigned short flags)
{
    struct kevent change;
    EV_SET(&change, ident, EVFILT_TIMER, EV_ADD | flags, 0, milliseconds, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1) {
        perror("kevent: watch health check timer");
        return false;
    }
    return true;
}

static bool start_health_probes(int queue, struct backend_pool *pool)
{
    expire_health_probes(pool);
    bool pending = false;
    for (size_t i = 0; i < pool->count; i++) {
        struct backend_target *target = &pool->targets[i];
        struct health_probe *probe = &target->probe;
        *probe = (struct health_probe){.fd = -1};
        int length = snprintf(probe->request, sizeof(probe->request),
                              "GET /health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n%s\r\n",
                              target->port, pool->origin_auth_header);
        if (length < 0 || (size_t)length >= sizeof(probe->request)) {
            target->healthy = false;
            continue;
        }
        probe->request_length = (size_t)length;
        probe->fd = connect_backend(target->port, &probe->connecting);
        if (probe->fd == -1) {
            target->healthy = false;
            continue;
        }
        // Health sockets use NULL event data so they cannot be mistaken for client connections.
        if (!set_socket_filter(queue, NULL, probe->fd, &probe->filter, EVFILT_WRITE)) {
            finish_health_probe(target, false);
            continue;
        }
        pending = true;
    }
    return !pending || watch_health_timer(queue, HEALTH_DEADLINE_TIMER, HEALTH_CHECK_TIMEOUT_MS, EV_ONESHOT);
}

static bool send_health_request(int queue, struct health_probe *probe, bool hangup)
{
    if (probe->connecting && !connection_ready(probe->fd)) {
        return false;
    }
    probe->connecting = false;
    ssize_t sent = send(probe->fd, probe->request + probe->request_sent,
                        probe->request_length - probe->request_sent, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    probe->request_sent += (size_t)sent;
    return probe->request_sent < probe->request_length ||
           set_socket_filter(queue, NULL, probe->fd, &probe->filter, EVFILT_READ);
}

static void service_health_event(int queue, struct backend_target *target, const struct kevent *event)
{
    struct health_probe *probe = &target->probe;
    if (event->filter != probe->filter) {
        return;
    }
    if (event->flags & EV_ERROR) {
        finish_health_probe(target, false);
        return;
    }
    bool hangup = (event->flags & EV_EOF) != 0;
    if (event->filter == EVFILT_WRITE) {
        if (!send_health_request(queue, probe, hangup)) {
            finish_health_probe(target, false);
        }
        return;
    }

    char bytes[1024];
    ssize_t received = recv(probe->fd, bytes, sizeof(bytes), 0);
    if (received <= 0) {
        if (received == 0 || !can_retry_socket_io(hangup)) {
            finish_health_probe(target, false);
        }
        return;
    }

    size_t consumed;
    enum proxy_header_result result = proxy_read_headers(probe->headers, &probe->header_length,
                                                       bytes, (size_t)received, &consumed);
    if (result != PROXY_HEADERS_MORE) {
        struct proxy_response response = {0};
        bool healthy = result == PROXY_HEADERS_READY &&
                       proxy_parse_response(probe->headers, false, (time_t)-1, &response) &&
                       response.status_code == 200;
        finish_health_probe(target, healthy);
    }
}

static void fail_backend(int queue, struct http_connection *client, int status)
{
    bool response_started = client->backend->response_started;
    close_backend(client);
    client->keep_alive = false;
    if (response_started || !queue_response(client, status) ||
        !set_client_filter(queue, client, EVFILT_WRITE) || !arm_timeout(queue, client)) {
        close_client(queue, client);
    }
}

static bool send_backend_request(int queue, struct http_connection *client, bool hangup)
{
    struct backend_connection *backend = client->backend;
    if (backend->connecting) {
        if (!connection_ready(backend->fd)) {
            return false;
        }
        backend->connecting = false;
    }
    size_t offset = backend->request_sent;
    const void *data;
    size_t length;
    if (offset < backend->request_header_length) {
        data = backend->request_headers + offset;
        length = backend->request_header_length - offset;
    } else {
        offset -= backend->request_header_length;
        if (offset >= client->request.content_length || client->request.body == NULL) {
            return false;
        }
        data = client->request.body + offset;
        length = client->request.content_length - offset;
    }
    ssize_t sent = send(backend->fd, data, length, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    backend->request_sent += (size_t)sent;
    size_t request_length = backend->request_header_length + client->request.content_length;
    return arm_timeout(queue, client) &&
           (backend->request_sent < request_length || set_backend_filter(queue, client, EVFILT_READ));
}

static bool tag_backend_response(struct backend_connection *backend)
{
    struct proxy_response *response = &backend->response;
    size_t offset = response->header_length - 2;
    size_t space = sizeof(response->headers) - offset;
    int length = snprintf(response->headers + offset, space,
                          "X-Backend-Port: %d\r\n\r\n", backend->port);
    if (length < 0 || (size_t)length >= space) {
        return false;
    }
    response->header_length = offset + (size_t)length;
    return true;
}

static bool buffer_backend_response(struct http_connection *client, size_t received)
{
    struct backend_connection *backend = client->backend;
    struct proxy_response *response = &backend->response;
    if (response->header_length == 0) {
        size_t consumed;
        enum proxy_header_result result = proxy_read_headers(backend->raw_headers,
            &backend->raw_header_length, client->response, received, &consumed);
        if (result == PROXY_HEADERS_INVALID) {
            return false;
        }
        if (result == PROXY_HEADERS_READY) {
            if (!proxy_parse_response(backend->raw_headers, client->keep_alive, (time_t)-1, response) ||
                !tag_backend_response(backend)) {
                return false;
            }
            client->keep_alive = response->client_keep_alive;
        }
        received -= consumed;
        memmove(client->response, client->response + consumed, received);
    }
    if (response->has_length) {
        if (received > response->remaining) {
            return false;
        }
        response->remaining -= received;
    }
    client->response_length = received;
    return true;
}

static void service_backend_event(int queue, struct http_connection *client,
                                  const struct kevent *event)
{
    struct backend_connection *backend = client->backend;
    if (event->filter != backend->filter) {
        return;
    }
    if (event->flags & EV_ERROR) {
        fail_backend(queue, client, 502);
        return;
    }
    bool hangup = (event->flags & EV_EOF) != 0;
    if (event->filter == EVFILT_WRITE) {
        if (!send_backend_request(queue, client, hangup)) {
            fail_backend(queue, client, 502);
        }
        return;
    }

    ssize_t received = recv(backend->fd, client->response, sizeof(client->response), 0);
    if (received > 0) {
        if (!buffer_backend_response(client, (size_t)received) ||
            // Forward the current buffer before reading more from the backend.
            (backend_response_pending(client) &&
             (!set_backend_filter(queue, client, 0) || !set_client_filter(queue, client, EVFILT_WRITE))) ||
            !arm_timeout(queue, client)) {
            fail_backend(queue, client, 502);
        }
    } else if (received == 0 && backend->response_started && !backend->response.has_length) {
        close_client(queue, client);
    } else if (received == 0 || !can_retry_socket_io(hangup)) {
        fail_backend(queue, client, 502);
    }
}

static void service_kqueue_client(int queue, const struct kevent *event)
{
    struct http_connection *client = event->udata;
    if (client->fd == -1 || event->ident != (uintptr_t)client->fd ||
        event->filter != client->filter) {
        return;
    }
    bool hangup = (event->flags & EV_EOF) != 0;
    if ((event->flags & EV_ERROR) || !handle_client(queue, client, hangup)) {
        close_client(queue, client);
        return;
    }

    short next_filter = client->response_length > 0 ? EVFILT_WRITE : EVFILT_READ;
    if (client->backend != NULL) {
        next_filter = backend_response_pending(client) ? EVFILT_WRITE : 0;
    } else if (client->file_pending) {
        next_filter = 0;
    }
    if (!set_client_filter(queue, client, next_filter)) {
        close_client(queue, client);
    }
}

static bool finish_file_job(struct http_connection *client, struct file_pool *pool,
                            struct file_job *job)
{
    client->file_pending = false;
    if (job->status != 200) {
        int status = job->status;
        file_pool_release(pool, job);
        // A failed read after the headers were sent closes the connection.
        return client->response_length == 0 && queue_response(client, status);
    }

    client->file = job;
    client->file_sent = 0;
    if (client->response_length > 0) {
        return true;
    }
    int length = http_write_file_headers(client->response, sizeof(client->response),
                                        client->request.version, job->content_type,
                                        (uintmax_t)job->remaining, "", client->keep_alive);
    return prepare_response(client, length, 200);
}

static bool service_file_completions(int queue, struct file_pool *pool,
                                     struct http_connection clients[])
{
    if (file_pool_drain_notifications(pool) == -1) {
        perror("read: file completion");
        return false;
    }
    struct file_job *job;
    while ((job = file_pool_take_completed(pool)) != NULL) {
        struct http_connection *client = &clients[job->slot];
        if (client->fd == -1 || client->connection_id != job->connection_id ||
            !client->file_pending) {
            file_pool_release(pool, job);
            continue;
        }
        if (!finish_file_job(client, pool, job) ||
            !set_client_filter(queue, client, EVFILT_WRITE) || !arm_timeout(queue, client)) {
            close_client(queue, client);
        }
    }
    return true;
}

static int run_server(int server_fd, struct backend_pool *origins, struct file_pool *file_pool)
{
    int queue = kqueue();
    if (queue == -1) {
        perror("kqueue");
        return 1;
    }
    if (fcntl(queue, F_SETFD, FD_CLOEXEC) == -1) {
        perror("fcntl: kqueue");
        close(queue);
        return 1;
    }
    struct kevent signals[2];
    EV_SET(&signals[0], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    EV_SET(&signals[1], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(queue, signals, 2, NULL, 0, NULL) == -1) {
        perror("kevent: watch signals");
        close(queue);
        return 1;
    }
    struct sigaction stop = {.sa_handler = handle_stop_signal};
    sigemptyset(&stop.sa_mask);
    if (sigaction(SIGINT, &stop, NULL) == -1 || sigaction(SIGTERM, &stop, NULL) == -1) {
        perror("sigaction");
        close(queue);
        return 1;
    }
    if (watch_read(queue, server_fd, NULL) == -1) {
        perror("kevent: watch server");
        close(queue);
        return 1;
    }
    int notification_fd = -1;
    if (file_pool != NULL) {
        notification_fd = file_pool_notification_fd(file_pool);
        if (watch_read(queue, notification_fd, NULL) == -1) {
            perror("kevent: watch file completion");
            close(queue);
            return 1;
        }
    }
    struct http_connection *clients = calloc(MAX_CLIENTS, sizeof(*clients));
    if (clients == NULL) {
        perror("calloc: client slots");
        close(queue);
        return 1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
    }
    for (size_t i = 0; i < origins->count; i++) {
        origins->targets[i].probe.fd = -1;
    }

    struct kevent events[MAX_EVENTS];
    int exit_status = 1;
    if (origins->health_interval_ms > 0 &&
        (!watch_health_timer(queue, HEALTH_INTERVAL_TIMER, origins->health_interval_ms, 0) ||
         !start_health_probes(queue, origins))) {
        goto close_connections;
    }
    for (;;) {
        int count = kevent(queue, NULL, 0, events, MAX_EVENTS, NULL);
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("kevent");
            break;
        }

        bool health_checks_due = false;
        // Apply expired timers before I/O can start another request in the same slot.
        for (int i = 0; i < count; i++) {
            const struct kevent *event = &events[i];
            if (event->filter == EVFILT_SIGNAL) {
                exit_status = 0;
                goto close_connections;
            }
            if (event->filter == EVFILT_TIMER) {
                if (event->ident == HEALTH_INTERVAL_TIMER) {
                    health_checks_due = true;
                    continue;
                }
                if (event->ident == HEALTH_DEADLINE_TIMER) {
                    expire_health_probes(origins);
                    continue;
                }
                struct http_connection *client = event->udata;
                if (!client->timer_armed) {
                    continue;
                }
                client->timer_armed = false;
                if (client->backend != NULL) {
                    fail_backend(queue, client, 504);
                } else {
                    close_client(queue, client);
                }
            }
        }

        const struct kevent *listener = NULL;
        for (int i = 0; i < count; i++) {
            const struct kevent *event = &events[i];
            if (event->filter == EVFILT_TIMER) {
                continue;
            }
            if (event->ident == (uintptr_t)server_fd) {
                listener = event;
            } else if (event->ident == (uintptr_t)notification_fd) {
                if (event->flags & (EV_ERROR | EV_EOF)) {
                    fputs("File completion pipe failed\n", stderr);
                    goto close_connections;
                }
                if (!service_file_completions(queue, file_pool, clients)) {
                    goto close_connections;
                }
            } else if (event->udata == NULL) {
                for (size_t i = 0; i < origins->count; i++) {
                    struct backend_target *target = &origins->targets[i];
                    if (event->ident == (uintptr_t)target->probe.fd) {
                        service_health_event(queue, target, event);
                        break;
                    }
                }
            } else {
                struct http_connection *client = event->udata;
                if (client->backend != NULL && event->ident == (uintptr_t)client->backend->fd) {
                    service_backend_event(queue, client, event);
                } else {
                    service_kqueue_client(queue, event);
                }
            }
        }

        // Reuse slots and probe descriptors only after handling the returned socket events.
        if (health_checks_due && !start_health_probes(queue, origins)) {
            break;
        }
        if (listener == NULL) {
            continue;
        }
        if (listener->flags & (EV_ERROR | EV_EOF)) {
            fputs("Listening socket failed\n", stderr);
            break;
        }

        for (int i = 0; i < ACCEPT_BATCH_SIZE; i++) {
            int slot = accept_client(server_fd, clients, origins, file_pool);
            if (slot == ACCEPT_FAILED) {
                goto close_connections;
            }
            if (slot == ACCEPT_RETRY) {
                break;
            }
            struct http_connection *client = &clients[slot];
            if (watch_read(queue, client->fd, client) == -1) {
                perror("kevent: watch client");
                close_client(queue, client);
            } else if (!arm_timeout(queue, client)) {
                close_client(queue, client);
            }
        }
    }

close_connections:
    expire_health_probes(origins);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            close_client(queue, &clients[i]);
        }
    }
    close(queue);
    free(clients);
    return exit_status;
}

static size_t backend_index(const struct backend_pool *pool, int port)
{
    for (size_t i = 0; i < pool->count; i++) {
        if (pool->targets[i].port == port) {
            return i;
        }
    }
    return pool->count;
}

static void add_default_route(struct backend_pool *pool, const char *path, int port, int standby_port)
{
    size_t index = backend_index(pool, port);
    if (index < pool->count) {
        size_t standby_index = backend_index(pool, standby_port);
        pool->routes[pool->route_count++] = (struct proxy_route){
            .path = path,
            .path_length = strlen(path),
            .backends = {.indices = {index}, .count = 1},
            .standby = standby_index < pool->count ? &pool->targets[standby_index] : NULL,
        };
    }
}

static bool set_route(struct backend_pool *pool, const char *argument)
{
    const char *separator = strchr(argument, '=');
    if (separator == NULL || *argument != '/') {
        return false;
    }
    for (const char *p = argument; p < separator; p++) {
        unsigned char byte = (unsigned char)*p;
        if (byte < 0x21 || byte > 0x7e || strchr("?#\\", byte) != NULL) {
            return false;
        }
    }

    struct backend_group group = {0};
    bool seen[MAX_BACKENDS] = {false};
    const char *value = separator + 1;
    for (;;) {
        if (*value < '0' || *value > '9') {
            return false;
        }
        char *end;
        long requested = strtol(value, &end, 10);
        if (requested < 1 || requested > UINT16_MAX || (*end != '\0' && *end != ',')) {
            return false;
        }
        size_t index = backend_index(pool, (int)requested);
        if (index == pool->count || seen[index]) {
            return false;
        }
        seen[index] = true;
        group.indices[group.count++] = index;
        if (*end == '\0') {
            break;
        }
        value = end + 1;
    }

    size_t path_length = (size_t)(separator - argument);
    struct proxy_route *route = find_route(pool, argument, path_length);
    if (route == NULL) {
        if (pool->route_count == MAX_ROUTES) {
            return false;
        }
        route = &pool->routes[pool->route_count++];
    }
    *route = (struct proxy_route){.path = argument, .path_length = path_length, .backends = group};
    return true;
}

int main(int argc, char *argv[])
{
    int port = LISTEN_PORT;
    struct backend_pool origins = {0};
    const char *route_arguments[MAX_ROUTES];
    size_t route_argument_count = 0;
    int option;
    while ((option = getopt(argc, argv, "p:b:H:r:")) == 'p' ||
           option == 'b' || option == 'H' || option == 'r') {
        if (option == 'r') {
            if (route_argument_count == MAX_ROUTES) {
                fprintf(stderr, "At most %d route arguments can be specified\n", MAX_ROUTES);
                return 1;
            }
            route_arguments[route_argument_count++] = optarg;
            continue;
        }
        char *end;
        long requested = strtol(optarg, &end, 10);
        long maximum = option == 'H' ? INT_MAX / 1000 : UINT16_MAX;
        if (*optarg < '0' || *optarg > '9' || *end != '\0' ||
            requested < 1 || requested > maximum) {
            if (option == 'H') {
                fprintf(stderr, "Health check interval must be an integer between 1 and %ld seconds\n", maximum);
            } else {
                fputs("Port must be an integer between 1 and 65535\n", stderr);
            }
            return 1;
        }
        if (option == 'p') {
            port = (int)requested;
        } else if (option == 'H') {
            origins.health_interval_ms = (int)requested * 1000;
        } else {
            if (origins.count == MAX_BACKENDS) {
                fprintf(stderr, "At most %d backend ports can be configured\n", MAX_BACKENDS);
                return 1;
            }
            origins.targets[origins.count++].port = (int)requested;
        }
    }
    if (option != -1 || argc - optind > 1) {
        fprintf(stderr, "Usage: %s [-p listen-port] [-b backend-port ...] [-H health-check-seconds]\n"
                        "       [-r /path=backend-port[,backend-port...] ...] [static-root]\n", argv[0]);
        return 1;
    }
    if (origins.count == 0) {
        origins.targets[0].port = DEFAULT_STATIC_PORT;
        origins.targets[1].port = DEFAULT_CGI_PORT;
        origins.count = 2;
    }
    for (size_t i = 0; i < origins.count; i++) {
        if (origins.targets[i].port == port) {
            fputs("A backend port cannot be the reverse proxy's listening port\n", stderr);
            return 1;
        }
        for (size_t j = 0; j < i; j++) {
            if (origins.targets[i].port == origins.targets[j].port) {
                fputs("Backend ports must be unique\n", stderr);
                return 1;
            }
        }
        origins.fallback.indices[origins.fallback.count++] = i;
    }
    add_default_route(&origins, "/static/index.html", DEFAULT_STATIC_PORT, 0);
    add_default_route(&origins, "/cgi-bin/sum", DEFAULT_CGI_PORT, DEFAULT_STATIC_PORT);
    for (size_t i = 0; i < route_argument_count; i++) {
        if (!set_route(&origins, route_arguments[i])) {
            fprintf(stderr, "Invalid route '%s'. Specify a path and unique, configured backend ports.\n",
                    route_arguments[i]);
            return 1;
        }
    }

    const char *origin_token = getenv(HTTP_ORIGIN_TOKEN_ENV);
    if (origin_token != NULL) {
        if (!http_origin_token_valid(origin_token)) {
            fprintf(stderr, "%s must contain %d hexadecimal characters\n",
                    HTTP_ORIGIN_TOKEN_ENV, HTTP_ORIGIN_TOKEN_LENGTH);
            return 1;
        }
        snprintf(origins.origin_auth_header, sizeof(origins.origin_auth_header),
                 HTTP_ORIGIN_TOKEN_HEADER ": %s\r\n", origin_token);
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return 1;
    }
    if (configure_socket(server_fd) == -1) {
        perror("fcntl");
        close(server_fd);
        return 1;
    }

    // Enable address reuse on the listening socket before bind().
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fputs("Invalid IPv4 address\n", stderr);
        close(server_fd);
        return 1;
    }

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, MAX_CLIENTS) == -1) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    int root_fd = -1;
    struct file_pool *file_pool = NULL;
    if (optind < argc) {
        const char *root = argv[optind];
        root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (root_fd == -1) {
            perror(root);
            close(server_fd);
            return 1;
        }
        file_pool = file_pool_create(root_fd, MAX_CLIENTS);
        if (file_pool == NULL) {
            perror("file worker pool");
            close(root_fd);
            close(server_fd);
            return 1;
        }
    }

    printf("Reverse proxy listening on http://127.0.0.1:%d\n", port);

    int result = run_server(server_fd, &origins, file_pool);
    if (file_pool != NULL) {
        file_pool_destroy(file_pool);
        close(root_fd);
    }
    close(server_fd);
    return result;
}
