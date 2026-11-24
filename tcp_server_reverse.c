#include "http.h"
#include "proxy_response.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
    MAX_BACKENDS = 16,
    MAX_CLIENTS = 64,
    MAX_EVENTS = MAX_CLIENTS * 3 + 3,
    BUFFER_SIZE = 4096,
    CLIENT_TIMEOUT_MS = 5000
};

struct backend_pool {
    int ports[MAX_BACKENDS];
    size_t count;
    size_t next;
};

struct backend_connection {
    int fd;
    int port;
    short filter;
    bool connecting;
    bool response_started;
    char request_headers[HTTP_MAX_HEADERS + 128];
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
    short filter;
    struct backend_pool *origins;
    struct backend_connection *backend;
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

static int next_backend(struct backend_pool *pool)
{
    int port = pool->ports[pool->next];
    pool->next = (pool->next + 1) % pool->count;
    return port;
}

static bool replaced_request_header(const char *name, size_t length)
{
    static const char *const names[] = {
        "Host", "Connection", "Content-Length", "Expect", "Keep-Alive", "Proxy-Connection",
        "Proxy-Authorization", "TE", "Trailer", "Transfer-Encoding", "Upgrade"
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
                          "%s %s %s\r\nHost: 127.0.0.1:%d\r\nContent-Length: %zu\r\nConnection: close\r\n",
                          request->method, request->uri, request->version, backend->port,
                          request->content_length);
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

static bool queue_response(struct http_connection *state, int status)
{
    int length = http_write_response(state->response, sizeof(state->response),
                                     state->request.version, status, "", "", state->keep_alive);
    if (length < 0 || (size_t)length >= sizeof(state->response)) {
        return false;
    }
    state->response_length = (size_t)length;
    state->response_sent = 0;
    state->interim_response = status == 100;
    return true;
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
    struct backend_connection *backend = calloc(1, sizeof(*backend));
    if (backend == NULL) {
        return queue_response(client, 500);
    }
    backend->port = next_backend(client->origins);
    if (!build_backend_request(client, backend)) {
        free(backend);
        return queue_response(client, 500);
    }

    backend->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend->fd == -1) {
        free(backend);
        return queue_response(client, 502);
    }
    client->backend = backend;

    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(backend->port)};
    if (configure_socket(backend->fd) == -1 ||
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        close_backend(client);
        return queue_response(client, 502);
    }
    if (connect(backend->fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        if (errno != EINPROGRESS && errno != EINTR) {
            close_backend(client);
            return queue_response(client, 502);
        }
        backend->connecting = true;
    }
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

static int accept_client(int server_fd, struct http_connection clients[], struct backend_pool *origins)
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
        .filter = EVFILT_READ,
        .origins = origins,
    };
    return slot;
}

static int watch_read(int queue, int fd, struct http_connection *client)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0, client);
    return kevent(queue, &change, 1, NULL, 0, NULL);
}

static bool set_socket_filter(int queue, struct http_connection *client, int fd,
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
        EV_SET(&changes[count], (uintptr_t)fd, next_filter, EV_ADD, 0, 0, client);
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
        int error;
        socklen_t length = sizeof(error);
        if (getsockopt(backend->fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1 || error != 0) {
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
    }
    if (!set_client_filter(queue, client, next_filter)) {
        close_client(queue, client);
    }
}

static int run_server(int server_fd, struct backend_pool *origins)
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
    struct http_connection clients[MAX_CLIENTS] = {0};
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
    }

    struct kevent events[MAX_EVENTS];
    int exit_status = 1;
    for (;;) {
        int count = kevent(queue, NULL, 0, events, MAX_EVENTS, NULL);
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("kevent");
            break;
        }

        // Apply expired timers before I/O can start another request in the same slot.
        for (int i = 0; i < count; i++) {
            const struct kevent *event = &events[i];
            if (event->filter == EVFILT_SIGNAL) {
                exit_status = 0;
                goto close_connections;
            }
            if (event->filter == EVFILT_TIMER) {
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
            } else {
                struct http_connection *client = event->udata;
                if (client->backend != NULL && event->ident == (uintptr_t)client->backend->fd) {
                    service_backend_event(queue, client, event);
                } else {
                    service_kqueue_client(queue, event);
                }
            }
        }

        // Process the returned client events before accepting into a freed slot.
        if (listener == NULL) {
            continue;
        }
        if (listener->flags & (EV_ERROR | EV_EOF)) {
            fputs("Listening socket failed\n", stderr);
            break;
        }

        int slot = accept_client(server_fd, clients, origins);
        if (slot == ACCEPT_FAILED) {
            break;
        }
        if (slot >= 0) {
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
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            close_client(queue, &clients[i]);
        }
    }
    close(queue);
    return exit_status;
}

int main(int argc, char *argv[])
{
    int port = LISTEN_PORT;
    struct backend_pool origins = {0};
    int option;
    while ((option = getopt(argc, argv, "p:b:")) == 'p' || option == 'b') {
        char *end;
        long requested = strtol(optarg, &end, 10);
        if (*optarg < '0' || *optarg > '9' || *end != '\0' ||
            requested < 1 || requested > UINT16_MAX) {
            fputs("Port must be an integer between 1 and 65535\n", stderr);
            return 1;
        }
        if (option == 'p') {
            port = (int)requested;
        } else {
            if (origins.count == MAX_BACKENDS) {
                fprintf(stderr, "At most %d backend ports can be configured\n", MAX_BACKENDS);
                return 1;
            }
            origins.ports[origins.count++] = (int)requested;
        }
    }
    if (option != -1 || optind != argc) {
        fprintf(stderr, "Usage: %s [-p listen-port] [-b backend-port ...]\n", argv[0]);
        return 1;
    }
    if (origins.count == 0) {
        origins = (struct backend_pool){.ports = {9000, 9001}, .count = 2};
    }
    for (size_t i = 0; i < origins.count; i++) {
        if (origins.ports[i] == port) {
            fputs("A backend port cannot be the reverse proxy's listening port\n", stderr);
            return 1;
        }
        for (size_t j = 0; j < i; j++) {
            if (origins.ports[i] == origins.ports[j]) {
                fputs("Backend ports must be unique\n", stderr);
                return 1;
            }
        }
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
    if (listen(server_fd, 8) == -1) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Reverse proxy listening on http://127.0.0.1:%d\n", port);

    int result = run_server(server_fd, &origins);
    close(server_fd);
    return result;
}
