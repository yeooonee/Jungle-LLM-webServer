#include "file_pool.h"
#include "http.h"
#include "proxy_cache.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    LISTEN_PORT = 9100,
    DEFAULT_ORIGIN_PORT = 9000,
    MAX_CLIENTS = 10000,
    ACCEPT_BATCH_SIZE = 64,
    MAX_EVENTS = MAX_CLIENTS * 3 + 4,
    BUFFER_SIZE = 4096,
    CLIENT_TIMEOUT_MS = 5000,
    PROXY_TIMEOUT_MS = 5000
};
#define HELLO_ETAG "\"hello-v1\""
#define STATIC_PREFIX "/static/"
#define STATIC_INDEX STATIC_PREFIX "index.html"
#define ORIGIN_ADDRESS "127.0.0.1"

struct upstream_connection {
    int fd;
    short filter;
    bool connecting;
    bool idle;
    bool reused;
    bool response_started;
    char request_headers[HTTP_MAX_HEADERS + 128];
    size_t request_header_length;
    size_t request_sent;
    char raw_headers[HTTP_MAX_HEADERS + 1];
    size_t raw_header_length;
    size_t header_sent;
    struct proxy_response response;
    struct proxy_cache_entry *cache_capture;
};

struct http_connection {
    int fd;
    int slot;
    int origin_port;
    uint64_t connection_id;
    short filter;
    char input[BUFFER_SIZE];
    size_t input_length;
    bool client_timer;
    char headers[HTTP_MAX_HEADERS + 1];
    size_t header_length;
    bool headers_complete;
    struct http_request request;
    size_t body_received;
    char response[HTTP_RESPONSE_SIZE];
    size_t response_length;
    size_t response_sent;
    bool interim_response;
    bool keep_alive;
    bool file_pending;
    struct file_job *file;
    size_t file_sent;
    struct upstream_connection *upstream;
    bool cache_checked;
    time_t cache_request_at;
    struct proxy_cache_hit cached;
    size_t cached_sent;
};

static bool arm_client_timeout(int queue, struct http_connection *client);

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

static bool set_client_filter(int queue, struct http_connection *client, short next_filter)
{
    return set_socket_filter(queue, client, client->fd, &client->filter, next_filter);
}

static bool set_upstream_filter(int queue, struct http_connection *client, short next_filter)
{
    struct upstream_connection *upstream = client->upstream;
    return set_socket_filter(queue, client, upstream->fd, &upstream->filter, next_filter);
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
            int status = http_parse_headers(state->headers, &state->request, HELLO_ETAG);
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

static const char *request_path(const char *uri)
{
    if (*uri != '/') {
        const char *scheme = strstr(uri, "://");
        if (scheme == NULL) {
            return "";
        }
        uri = scheme + 3;
        uri += strcspn(uri, "/?");
    }
    return uri;
}

static bool request_path_is(const char *path, const char *expected)
{
    size_t length = strcspn(path, "?");
    return strlen(expected) == length && memcmp(path, expected, length) == 0;
}

static bool is_offloaded_asset(const char *path)
{
    return request_path_is(path, STATIC_PREFIX "logo.png") ||
           request_path_is(path, STATIC_PREFIX "style.css");
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

static int submit_static_file(struct http_connection *state, struct file_pool *pool, const char *path)
{
    if (pool == NULL) {
        return 404;
    }
    int status = file_pool_submit_open(pool, state->slot, state->connection_id,
                                       path + sizeof(STATIC_PREFIX) - 1);
    state->file_pending = status == 0;
    return status;
}

static bool queue_response(struct http_connection *state, struct file_pool *pool, int status)
{
    const char *body = "";
    const char *extra_headers = "";
    if (status == 200 && strcmp(state->request.method, "GET") == 0) {
        const char *path = request_path(state->request.uri);
        if (strncmp(path, STATIC_PREFIX, sizeof(STATIC_PREFIX) - 1) == 0) {
            status = is_offloaded_asset(path) ? submit_static_file(state, pool, path) : 404;
            if (status == 0) {
                return true;
            }
        } else if (request_path_is(path, "/redirect/permanent")) {
            status = 301;
            extra_headers = "Location: /\r\n";
        } else if (request_path_is(path, "/redirect")) {
            status = 302;
            extra_headers = "Location: /\r\n";
        } else {
            extra_headers = "ETag: " HELLO_ETAG "\r\n";
            if (state->request.etag_matches) {
                status = 304;
            } else {
                body = "hello";
            }
        }
    } else if (status == 200 && strcmp(state->request.method, "POST") == 0 &&
               request_path_is(request_path(state->request.uri), "/redirect/after-post")) {
        status = 303;
        extra_headers = "Location: /\r\n";
    }
    int length = http_write_response(state->response, sizeof(state->response),
                                     state->request.version, status, body, extra_headers,
                                     state->keep_alive);
    return prepare_response(state, length, status);
}

static bool arm_upstream_timeout(int queue, struct http_connection *client)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)(MAX_CLIENTS + client->slot), EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           0, PROXY_TIMEOUT_MS, client);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1) {
        perror("kevent: watch upstream timeout");
        return false;
    }
    return true;
}

static void cancel_upstream_timeout(int queue, struct http_connection *client)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)(MAX_CLIENTS + client->slot), EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1 && errno != ENOENT) {
        perror("kevent: remove upstream timeout");
    }
}

static void close_upstream(int queue, struct http_connection *client)
{
    if (client->upstream == NULL) {
        return;
    }
    if (!client->upstream->idle) {
        cancel_upstream_timeout(queue, client);
    }
    if (client->upstream->fd != -1) {
        close(client->upstream->fd);
    }
    proxy_cache_release(client->upstream->cache_capture);
    free(client->upstream);
    client->upstream = NULL;
}

static bool origin_socket_is_idle(int fd)
{
    char byte;
    ssize_t received;
    do {
        received = recv(fd, &byte, 1, MSG_PEEK);
    } while (received == -1 && errno == EINTR);
    // A reusable socket has neither unread response data nor an EOF.
    return received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static int open_origin(struct upstream_connection *upstream, int port)
{
    struct sockaddr_in origin = {0};
    origin.sin_family = AF_INET;
    origin.sin_port = htons(port);
    if (inet_pton(AF_INET, ORIGIN_ADDRESS, &origin.sin_addr) != 1) {
        return 500;
    }
    upstream->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (upstream->fd == -1 || configure_socket(upstream->fd) == -1) {
        perror("open upstream socket");
        return 502;
    }
    if (connect(upstream->fd, (struct sockaddr *)&origin, sizeof(origin)) == -1) {
        if (errno != EINPROGRESS && errno != EINTR) {
            fprintf(stderr, "connect to upstream port %d: %s\n", port, strerror(errno));
            return 502;
        }
        upstream->connecting = true;
    }
    return 0;
}

static bool prepare_cached_response(int queue, struct http_connection *client, bool keep_alive)
{
    int length = snprintf(client->response, sizeof(client->response),
                          "Age: %" PRIuMAX "\r\nConnection: %s\r\n\r\n", client->cached.age,
                          keep_alive ? "keep-alive" : "close");
    client->keep_alive = keep_alive;
    return prepare_response(client, length, 200) && arm_client_timeout(queue, client);
}

static bool build_upstream_request(struct upstream_connection *upstream,
                                   const struct http_request *request,
                                   const char *path, const char *authority, bool is_post)
{
    char *headers = upstream->request_headers;
    size_t capacity = sizeof(upstream->request_headers);
    const char *slash = *path == '/' ? "" : "/";
    int length = snprintf(headers, capacity, "%s %s%s %s\r\nHost: %s\r\nConnection: keep-alive\r\n",
                          request->method, slash, path, request->version, authority);
    if (length < 0 || (size_t)length >= capacity) {
        return false;
    }
    size_t used = (size_t)length;
    if (is_post) {
        const char *content_type = request->content_type;
        length = snprintf(headers + used, capacity - used, "Content-Length: %zu\r\n%s%s%s",
                          request->content_length, content_type ? "Content-Type: " : "",
                          content_type ? content_type : "", content_type ? "\r\n" : "");
        if (length < 0 || (size_t)length >= capacity - used) {
            return false;
        }
        used += (size_t)length;
    }
    if (capacity - used < 2) {
        return false;
    }
    memcpy(headers + used, "\r\n", 2);
    upstream->request_header_length = used + 2;
    return true;
}

static bool start_upstream(int queue, struct http_connection *state, struct file_pool *pool)
{
    bool client_keep_alive = state->keep_alive;
    state->keep_alive = false;
    bool is_get = strcmp(state->request.method, "GET") == 0;
    bool is_post = strcmp(state->request.method, "POST") == 0;
    if (!is_get && !is_post) {
        int length = http_write_response(state->response, sizeof(state->response),
                                         state->request.version, 405, "", "Allow: GET, POST\r\n",
                                         false);
        return prepare_response(state, length, 405);
    }

    const char *uri = state->request.uri;
    if (strncmp(uri, "http://", sizeof("http://") - 1) != 0) {
        return queue_response(state, pool, 501);
    }
    if (is_get && state->request.content_length != 0) {
        return queue_response(state, pool, 400);
    }
    char authority[sizeof("127.0.0.1:65535")];
    snprintf(authority, sizeof(authority), "%s:%d", ORIGIN_ADDRESS, state->origin_port);
    const char *host = uri + sizeof("http://") - 1;
    size_t host_length = strcspn(host, "/?");
    if (host_length != strlen(authority) || memcmp(host, authority, host_length) != 0) {
        return queue_response(state, pool, host_length == 0 ? 400 : 403);
    }

    struct sockaddr_in local = {0};
    socklen_t local_length = sizeof(local);
    if (getsockname(state->fd, (struct sockaddr *)&local, &local_length) == -1) {
        return queue_response(state, pool, 500);
    }
    if (ntohs(local.sin_port) == state->origin_port) {
        return queue_response(state, pool, 403);
    }

    const char *path = host + host_length;
    if (is_get && pool != NULL && is_offloaded_asset(path)) {
        state->keep_alive = client_keep_alive;
        int status = submit_static_file(state, pool, path);
        return status == 0 || queue_response(state, pool, status);
    }

    if (!state->cache_checked) {
        state->cache_checked = true;
        state->cache_request_at = (time_t)-1;
        // Keep the HTML request on the origin path when static offloading is enabled.
        bool index_from_origin = pool != NULL && request_path_is(path, STATIC_INDEX);
        if (is_get && !index_from_origin && !state->request.bypass_cache &&
            strcmp(state->request.version, "HTTP/1.1") == 0) {
            time_t now = proxy_cache_now();
            if (now >= 0) {
                if (proxy_cache_lookup(uri, now, &state->cached)) {
                    return prepare_cached_response(queue, state, client_keep_alive);
                }
                if (proxy_cache_note_request(uri, now)) {
                    state->cache_request_at = now;
                }
            }
        }
    }

    if (state->upstream != NULL && !origin_socket_is_idle(state->upstream->fd)) {
        close_upstream(queue, state);
    }
    struct upstream_connection *upstream = state->upstream;
    if (upstream == NULL) {
        upstream = calloc(1, sizeof(*upstream));
        if (upstream == NULL) {
            return queue_response(state, pool, 500);
        }
        upstream->fd = -1;
        state->upstream = upstream;
        int status = open_origin(upstream, state->origin_port);
        if (status != 0) {
            close_upstream(queue, state);
            return queue_response(state, pool, status);
        }
    } else {
        upstream->reused = true;
    }
    upstream->idle = false;
    upstream->response_started = false;
    upstream->request_sent = 0;
    upstream->raw_header_length = 0;
    upstream->header_sent = 0;
    upstream->response = (struct proxy_response){0};

    if (!build_upstream_request(upstream, &state->request, path, authority, is_post)) {
        close_upstream(queue, state);
        return queue_response(state, pool, 500);
    }
    if (!set_upstream_filter(queue, state, EVFILT_WRITE) || !arm_upstream_timeout(queue, state)) {
        close_upstream(queue, state);
        return queue_response(state, pool, 500);
    }
    state->keep_alive = client_keep_alive;
    return true;
}

static void reset_request(struct http_connection *state, struct file_pool *pool)
{
    proxy_cache_release(state->cached.entry);
    state->cached = (struct proxy_cache_hit){0};
    state->cached_sent = 0;
    state->cache_checked = false;
    free(state->request.body);
    state->request = (struct http_request){0};
    if (state->file != NULL) {
        file_pool_release(pool, state->file);
        state->file = NULL;
    }
    state->header_length = 0;
    state->headers_complete = false;
    state->body_received = 0;
    state->response_length = 0;
    state->response_sent = 0;
    state->interim_response = false;
    state->keep_alive = false;
    state->file_pending = false;
    state->file_sent = 0;
}

static bool process_client_input(int queue, struct http_connection *state, struct file_pool *pool)
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
    if (status == 200 || status == 405) {
        if (state->request.method == NULL || state->request.uri == NULL ||
            state->request.version == NULL) {
            return queue_response(state, pool, 500);
        }
        state->keep_alive = status == 200 && !state->request.connection_close &&
                            (strcmp(state->request.version, "HTTP/1.1") == 0 ||
                             state->request.connection_keep_alive);
        if (state->request.uri[0] != '/') {
            return start_upstream(queue, state, pool);
        }
    }
    close_upstream(queue, state);
    return queue_response(state, pool, status);
}

static void cancel_client_timeout(int queue, struct http_connection *state)
{
    if (!state->client_timer) {
        return;
    }
    struct kevent change;
    EV_SET(&change, (uintptr_t)state->slot, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1 && errno != ENOENT) {
        perror("kevent: remove client timeout");
    }
    state->client_timer = false;
}

static bool arm_client_timeout(int queue, struct http_connection *state)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)state->slot, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           0, CLIENT_TIMEOUT_MS, state);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1) {
        perror("kevent: watch client timeout");
        return false;
    }
    state->client_timer = true;
    return true;
}

static bool finish_response(int queue, struct http_connection *state, struct file_pool *pool)
{
    if (!state->keep_alive) {
        return false;
    }
    cancel_client_timeout(queue, state);
    reset_request(state, pool);
    if (state->input_length > 0) {
        return process_client_input(queue, state, pool);
    }
    return arm_client_timeout(queue, state);
}

static bool can_retry_socket_io(bool hangup)
{
    return errno == EINTR ||
           ((errno == EAGAIN || errno == EWOULDBLOCK) && !hangup);
}

static bool upstream_response_pending(const struct http_connection *client)
{
    return client->upstream->header_sent < client->upstream->response.header_length ||
           client->response_sent < client->response_length;
}

static bool send_cached_response(int queue, struct http_connection *client,
                                 struct file_pool *pool, bool hangup)
{
    const struct proxy_cache_hit *hit = &client->cached;
    const char *data = hit->headers;
    size_t length = hit->header_length;
    size_t offset = client->cached_sent;
    if (offset >= length) {
        offset -= length;
        data = client->response;
        length = client->response_length;
        if (offset >= length) {
            offset -= length;
            data = hit->body;
            length = hit->body_length;
        }
    }

    ssize_t sent = send(client->fd, data + offset, length - offset, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    client->cached_sent += (size_t)sent;
    if (client->cached_sent == hit->header_length + client->response_length + hit->body_length) {
        return finish_response(queue, client, pool);
    }
    return arm_client_timeout(queue, client);
}

static bool send_upstream_response(int queue, struct http_connection *client,
                                   struct file_pool *pool, bool hangup)
{
    struct upstream_connection *upstream = client->upstream;
    bool sending_headers = upstream->header_sent < upstream->response.header_length;
    const char *buffer = sending_headers ? upstream->response.headers : client->response;
    size_t length = sending_headers ? upstream->response.header_length : client->response_length;
    size_t *sent_so_far = sending_headers ? &upstream->header_sent : &client->response_sent;

    ssize_t sent = send(client->fd, buffer + *sent_so_far, length - *sent_so_far, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    upstream->response_started = true;
    *sent_so_far += (size_t)sent;
    if (upstream_response_pending(client)) {
        return arm_upstream_timeout(queue, client);
    }

    client->response_length = 0;
    client->response_sent = 0;
    if (!upstream->response.has_length || upstream->response.remaining != 0) {
        return set_upstream_filter(queue, client, EVFILT_READ) && arm_upstream_timeout(queue, client);
    }

    // Only park the socket once the complete response has also reached the client.
    if (client->keep_alive && upstream->response.origin_keep_alive &&
        set_upstream_filter(queue, client, EVFILT_READ)) {
        cancel_upstream_timeout(queue, client);
        upstream->idle = true;
    } else {
        close_upstream(queue, client);
    }
    return finish_response(queue, client, pool);
}

static bool send_file_chunk(int queue, struct http_connection *state, struct file_pool *pool,
                            bool hangup)
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
        return finish_response(queue, state, pool);
    }
    if (state->file_sent == job->length) {
        state->file = NULL;
        state->file_pending = true;
        file_pool_submit_read(pool, job);
    }
    return true;
}

static bool handle_client(int queue, struct http_connection *state, struct file_pool *pool,
                          bool hangup)
{
    if (state->cached.entry != NULL) {
        return send_cached_response(queue, state, pool, hangup);
    }
    if (state->upstream != NULL && !state->upstream->idle) {
        return send_upstream_response(queue, state, pool, hangup);
    }
    if (state->response_length == 0) {
        ssize_t received = recv(state->fd, state->input + state->input_length,
                                sizeof(state->input) - state->input_length, 0);
        if (received > 0) {
            cancel_client_timeout(queue, state);
            state->input_length += (size_t)received;
            return process_client_input(queue, state, pool);
        }
        if (received == 0) {
            return state->header_length > 0 && queue_response(state, pool, 400);
        }
        return can_retry_socket_io(hangup);
    }

    if (state->response_sent == state->response_length) {
        return send_file_chunk(queue, state, pool, hangup);
    }

    ssize_t sent = send(state->fd, state->response + state->response_sent,
                        state->response_length - state->response_sent, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    state->response_sent += (size_t)sent;
    if (state->response_sent < state->response_length) {
        return true;
    }
    if (state->interim_response) {
        // After 100 Continue, resume reading this request's body.
        state->response_length = 0;
        state->response_sent = 0;
        state->interim_response = false;
        return true;
    }
    if (state->file != NULL && state->file->remaining > 0) {
        return true;
    }
    return finish_response(queue, state, pool);
}

static void close_client(int queue, struct http_connection *client, struct file_pool *pool)
{
    cancel_client_timeout(queue, client);
    close_upstream(queue, client);
    reset_request(client, pool);
    close(client->fd);
    client->fd = -1;
}

enum { ACCEPT_RETRY = -1, ACCEPT_FAILED = -2 };

static int accept_client(int server_fd, struct http_connection clients[], int origin_port)
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
        .origin_port = origin_port,
        .connection_id = clients[slot].connection_id + 1,
        .filter = EVFILT_READ,
    };
    return slot;
}

static int watch_read(int queue, int fd, struct http_connection *client)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0, client);
    return kevent(queue, &change, 1, NULL, 0, NULL);
}

static void fail_upstream(int queue, struct file_pool *pool, struct http_connection *client,
                          int status)
{
    bool response_started = client->upstream->response_started;
    close_upstream(queue, client);
    client->keep_alive = false;
    if (response_started || !queue_response(client, pool, status) ||
        !set_client_filter(queue, client, EVFILT_WRITE)) {
        close_client(queue, client, pool);
    }
}

static void upstream_disconnected(int queue, struct file_pool *pool, struct http_connection *client)
{
    struct upstream_connection *upstream = client->upstream;
    if (!upstream->reused || upstream->raw_header_length != 0 ||
        strcmp(client->request.method, "GET") != 0) {
        fail_upstream(queue, pool, client, 502);
        return;
    }
    // The origin may close just after the idle probe. Retry the GET once on a fresh socket.
    close_upstream(queue, client);
    if (!start_upstream(queue, client, pool) ||
        (client->upstream == NULL && !set_client_filter(queue, client, EVFILT_WRITE))) {
        close_client(queue, client, pool);
    }
}

static bool send_upstream_request(int queue, struct http_connection *client, bool hangup)
{
    struct upstream_connection *upstream = client->upstream;
    if (upstream->connecting) {
        int error;
        socklen_t length = sizeof(error);
        if (getsockopt(upstream->fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1) {
            perror("getsockopt: upstream connection");
            return false;
        }
        if (error != 0) {
            fprintf(stderr, "upstream connection: %s\n", strerror(error));
            return false;
        }
        upstream->connecting = false;
    }

    size_t offset = upstream->request_sent;
    const void *data;
    size_t length;
    if (offset < upstream->request_header_length) {
        data = upstream->request_headers + offset;
        length = upstream->request_header_length - offset;
    } else {
        offset -= upstream->request_header_length;
        if (offset >= client->request.content_length || client->request.body == NULL) {
            return false;
        }
        data = client->request.body + offset;
        length = client->request.content_length - offset;
    }
    ssize_t sent = send(upstream->fd, data, length, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    upstream->request_sent += (size_t)sent;
    size_t request_length = upstream->request_header_length + client->request.content_length;
    return arm_upstream_timeout(queue, client) &&
           (upstream->request_sent < request_length ||
            set_upstream_filter(queue, client, EVFILT_READ));
}

static bool buffer_upstream_response(struct http_connection *client, size_t received)
{
    struct upstream_connection *upstream = client->upstream;
    struct proxy_response *response = &upstream->response;
    if (response->header_length == 0) {
        size_t consumed;
        enum proxy_header_result result = proxy_read_headers(upstream->raw_headers,
            &upstream->raw_header_length, client->response, received, &consumed);
        if (result == PROXY_HEADERS_INVALID) {
            return false;
        }
        if (result == PROXY_HEADERS_READY) {
            if (!proxy_parse_response(upstream->raw_headers, client->keep_alive,
                                      client->cache_request_at, response)) {
                return false;
            }
            client->keep_alive = response->client_keep_alive;
            upstream->cache_capture = proxy_cache_begin(client->request.uri, response);
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
    proxy_cache_append(&upstream->cache_capture, client->response, received);
    client->response_length = received;
    return true;
}

static void service_upstream_event(int queue, struct file_pool *pool,
                                   struct http_connection *client, const struct kevent *event)
{
    struct upstream_connection *upstream = client->upstream;
    if (event->filter != upstream->filter) {
        return;
    }
    if (upstream->idle) {
        // Readability here means a closed connection or unexpected bytes between responses.
        close_upstream(queue, client);
        return;
    }
    if (event->flags & EV_ERROR) {
        upstream_disconnected(queue, pool, client);
        return;
    }
    bool hangup = (event->flags & EV_EOF) != 0;
    if (event->filter == EVFILT_WRITE) {
        if (!send_upstream_request(queue, client, hangup)) {
            upstream_disconnected(queue, pool, client);
        }
        return;
    }

    ssize_t received = recv(upstream->fd, client->response, sizeof(client->response), 0);
    if (received > 0) {
        if (!buffer_upstream_response(client, (size_t)received) ||
            // Pause reads while the client has headers or a body chunk to send.
            (upstream_response_pending(client) &&
             (!set_upstream_filter(queue, client, 0) ||
              !set_client_filter(queue, client, EVFILT_WRITE))) ||
            !arm_upstream_timeout(queue, client)) {
            fail_upstream(queue, pool, client, 502);
        }
    } else if (received == 0 && upstream->response_started && !upstream->response.has_length) {
        close_client(queue, client, pool);
    } else if (received == 0 || !can_retry_socket_io(hangup)) {
        upstream_disconnected(queue, pool, client);
    }
}

static void service_kqueue_client(int queue, struct file_pool *pool, const struct kevent *event)
{
    struct http_connection *client = event->udata;
    if (client->fd == -1 || event->ident != (uintptr_t)client->fd ||
        event->filter != client->filter) {
        return;
    }
    bool hangup = (event->flags & EV_EOF) != 0;
    if ((event->flags & EV_ERROR) || !handle_client(queue, client, pool, hangup)) {
        close_client(queue, client, pool);
        return;
    }

    short next_filter = EVFILT_READ;
    if (client->upstream != NULL && !client->upstream->idle) {
        next_filter = upstream_response_pending(client) ? EVFILT_WRITE : 0;
    } else if (client->file_pending) {
        next_filter = 0;
    } else if (client->response_length > 0) {
        next_filter = EVFILT_WRITE;
    }
    if (!set_client_filter(queue, client, next_filter)) {
        close_client(queue, client, pool);
    }
}

static bool finish_file_job(struct http_connection *client, struct file_pool *pool,
                            struct file_job *job)
{
    client->file_pending = false;
    if (job->status != 200) {
        int status = job->status;
        file_pool_release(pool, job);
        // Once the headers have been sent, a failed read ends the response.
        return client->response_length == 0 && queue_response(client, pool, status);
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
            !set_client_filter(queue, client, EVFILT_WRITE)) {
            close_client(queue, client, pool);
        }
    }
    return true;
}

static int run_server(int server_fd, struct file_pool *pool, int origin_port)
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
    if (pool != NULL) {
        notification_fd = file_pool_notification_fd(pool);
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

        // Resolve expired timers before socket events can start a new request in this slot.
        for (int i = 0; i < count; i++) {
            const struct kevent *event = &events[i];
            if (event->filter == EVFILT_SIGNAL) {
                exit_status = 0;
                goto close_connections;
            }
            if (event->filter == EVFILT_TIMER) {
                struct http_connection *client = event->udata;
                if (event->ident >= MAX_CLIENTS) {
                    if (client->upstream != NULL && !client->upstream->idle) {
                        fail_upstream(queue, pool, client, 504);
                    }
                } else if (client->client_timer) {
                    client->client_timer = false;
                    close_client(queue, client, pool);
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
                if (event->flags & EV_ERROR) {
                    fputs("File completion pipe failed\n", stderr);
                    goto close_connections;
                }
                if (!service_file_completions(queue, pool, clients)) {
                    goto close_connections;
                }
            } else {
                struct http_connection *client = event->udata;
                if (client->upstream != NULL && event->ident == (uintptr_t)client->upstream->fd) {
                    service_upstream_event(queue, pool, client, event);
                } else {
                    service_kqueue_client(queue, pool, event);
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

        for (int i = 0; i < ACCEPT_BATCH_SIZE; i++) {
            int slot = accept_client(server_fd, clients, origin_port);
            if (slot == ACCEPT_FAILED) {
                goto close_connections;
            }
            if (slot == ACCEPT_RETRY) {
                break;
            }
            if (watch_read(queue, clients[slot].fd, &clients[slot]) == -1) {
                perror("kevent: watch client");
                close_client(queue, &clients[slot], pool);
            }
        }
    }

close_connections:
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            close_client(queue, &clients[i], pool);
        }
    }
    proxy_cache_destroy();
    close(queue);
    free(clients);
    return exit_status;
}

int main(int argc, char *argv[])
{
    int port = LISTEN_PORT;
    int origin_port = DEFAULT_ORIGIN_PORT;
    int option;
    while ((option = getopt(argc, argv, "p:o:")) == 'p' || option == 'o') {
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
            origin_port = (int)requested;
        }
    }
    if (option != -1 || argc - optind > 1) {
        fprintf(stderr, "Usage: %s [-p port] [-o destination-port] [static-root]\n", argv[0]);
        return 1;
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
    struct file_pool *pool = NULL;
    if (optind < argc) {
        const char *root = argv[optind];
        root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (root_fd == -1) {
            perror(root);
            close(server_fd);
            return 1;
        }
        pool = file_pool_create(root_fd, MAX_CLIENTS);
        if (pool == NULL) {
            perror("file worker pool");
            close(root_fd);
            close(server_fd);
            return 1;
        }
    }

    printf("HTTP server listening on http://127.0.0.1:%d\n", port);

    int result = run_server(server_fd, pool, origin_port);
    if (pool != NULL) {
        file_pool_destroy(pool);
        close(root_fd);
    }
    close(server_fd);
    return result;
}
