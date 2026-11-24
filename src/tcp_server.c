#include "cgi.h"
#include "file_pool.h"
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    MAX_CLIENTS = 64,
    MAX_EVENTS = MAX_CLIENTS * 2 + 5,
    BUFFER_SIZE = 4096,
    KEEP_ALIVE_TIMEOUT_MS = 5000
};
#define HELLO_ETAG "\"hello-v1\""
#define STATIC_PREFIX "/static/"
#define CGI_PREFIX "/cgi-bin/"

struct http_connection {
    int fd;
    int slot;
    const char *required_origin_token;
    uint64_t connection_id;
    short filter;
    char input[BUFFER_SIZE];
    size_t input_length;
    bool keep_alive_timer;
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
    struct cgi_process *cgi;
};

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

static bool origin_request_allowed(const char *expected, const char *received)
{
    if (expected == NULL) {
        return true;
    }
    if (received == NULL || strlen(received) != HTTP_ORIGIN_TOKEN_LENGTH) {
        return false;
    }
    volatile unsigned char difference = 0;
    for (size_t i = 0; i < HTTP_ORIGIN_TOKEN_LENGTH; i++) {
        difference |= (unsigned char)expected[i] ^ (unsigned char)received[i];
    }
    return difference == 0;
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
            if (!origin_request_allowed(state->required_origin_token, state->request.origin_token)) {
                return 403;
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

static bool queue_response(struct http_connection *state, struct file_pool *pool, int status)
{
    const char *body = "";
    const char *extra_headers = "";
    if (status == 200 && strcmp(state->request.method, "GET") == 0) {
        const char *path = request_path(state->request.uri);
        size_t prefix_length = sizeof(STATIC_PREFIX) - 1;
        if (strncmp(path, STATIC_PREFIX, prefix_length) == 0) {
            if (pool == NULL) {
                status = 404;
            } else {
                status = file_pool_submit_open(pool, state->slot, state->connection_id,
                                               path + prefix_length);
            }
            if (status == 0) {
                state->file_pending = true;
                return true;
            }
        } else if (request_path_is(path, "/health")) {
            body = "ok";
            extra_headers = "Cache-Control: no-store\r\n";
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

static bool dispatch_request(int queue, struct http_connection *state,
                             struct file_pool *pool, int status)
{
    if (status != 200 && status != 405) {
        return queue_response(state, pool, status);
    }
    const char *path = request_path(state->request.uri);
    if (strncmp(path, CGI_PREFIX, sizeof(CGI_PREFIX) - 1) != 0) {
        return queue_response(state, pool, status);
    }
    if (!request_path_is(path, CGI_PREFIX "sum")) {
        return queue_response(state, pool, 404);
    }
    if (strcmp(state->request.method, "GET") != 0) {
        int length = http_write_response(state->response, sizeof(state->response),
                                         state->request.version, 405, "", "Allow: GET\r\n",
                                         state->keep_alive);
        return prepare_response(state, length, 405);
    }

    struct sockaddr_in local_address = {0};
    socklen_t address_length = sizeof(local_address);
    if (getsockname(state->fd, (struct sockaddr *)&local_address, &address_length) == -1) {
        return queue_response(state, pool, 500);
    }
    const char *query = strchr(path, '?');
    struct cgi_process *cgi = cgi_start_sum(query == NULL ? "" : query + 1,
                                           state->request.version, ntohs(local_address.sin_port));
    if (cgi == NULL) {
        return queue_response(state, pool, 500);
    }
    struct kevent changes[2];
    EV_SET(&changes[0], (uintptr_t)cgi->output_fd, EVFILT_READ, EV_ADD, 0, 0, state);
    EV_SET(&changes[1], (uintptr_t)state->slot, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           0, CGI_TIMEOUT_MS, state);
    if (kevent(queue, changes, 2, NULL, 0, NULL) == -1) {
        perror("kevent: watch CGI");
        cgi_dispose(cgi);
        return queue_response(state, pool, 500);
    }
    state->cgi = cgi;
    return true;
}

static void reset_request(struct http_connection *state, struct file_pool *pool)
{
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
    if ((status == 200 || status == 405) &&
        (state->request.method == NULL || state->request.uri == NULL ||
         state->request.version == NULL)) {
        return queue_response(state, pool, 500);
    }
    if (status == 200) {
        state->keep_alive = !state->request.connection_close &&
                            (strcmp(state->request.version, "HTTP/1.1") == 0 ||
                             state->request.connection_keep_alive);
    }
    return dispatch_request(queue, state, pool, status);
}

static void remove_timer(int queue, uintptr_t ident)
{
    struct kevent change;
    EV_SET(&change, ident, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1 && errno != ENOENT) {
        perror("kevent: remove timer");
    }
}

static void cancel_keep_alive_timer(int queue, struct http_connection *state)
{
    if (state->keep_alive_timer) {
        remove_timer(queue, (uintptr_t)(MAX_CLIENTS + state->slot));
        state->keep_alive_timer = false;
    }
}

static bool finish_response(int queue, struct http_connection *state, struct file_pool *pool)
{
    if (!state->keep_alive) {
        return false;
    }
    reset_request(state, pool);
    if (state->input_length > 0) {
        return process_client_input(queue, state, pool);
    }

    // CGI timers use slot numbers. Give keep-alive timers a separate range.
    struct kevent change;
    EV_SET(&change, (uintptr_t)(MAX_CLIENTS + state->slot), EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           0, KEEP_ALIVE_TIMEOUT_MS, state);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1) {
        perror("kevent: watch keep-alive timeout");
        return false;
    }
    state->keep_alive_timer = true;
    return true;
}

static bool can_retry_socket_io(bool hangup)
{
    return errno == EINTR ||
           ((errno == EAGAIN || errno == EWOULDBLOCK) && !hangup);
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
    if (state->response_length == 0) {
        ssize_t received = recv(state->fd, state->input + state->input_length,
                                sizeof(state->input) - state->input_length, 0);
        if (received > 0) {
            cancel_keep_alive_timer(queue, state);
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

static void discard_cgi(int queue, struct http_connection *client)
{
    if (client->cgi == NULL) {
        return;
    }
    remove_timer(queue, (uintptr_t)client->slot);
    cgi_dispose(client->cgi);
    client->cgi = NULL;
}

static void close_client(int queue, struct http_connection *client, struct file_pool *pool)
{
    cancel_keep_alive_timer(queue, client);
    reset_request(client, pool);
    discard_cgi(queue, client);
    close(client->fd);
    client->fd = -1;
}

enum { ACCEPT_RETRY = -1, ACCEPT_FAILED = -2 };

static int accept_client(int server_fd, struct http_connection clients[], const char *origin_token)
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
        .required_origin_token = origin_token,
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

static bool set_client_filter(int queue, struct http_connection *client, short next_filter)
{
    if (client->filter == next_filter) {
        return true;
    }
    struct kevent changes[2];
    int count = 0;
    if (client->filter != 0) {
        EV_SET(&changes[count], (uintptr_t)client->fd, client->filter, EV_DELETE, 0, 0, NULL);
        count++;
    }
    if (next_filter != 0) {
        EV_SET(&changes[count], (uintptr_t)client->fd, next_filter, EV_ADD, 0, 0, client);
        count++;
    }
    if (kevent(queue, changes, count, NULL, 0, NULL) == -1) {
        perror("kevent: change client filter");
        return false;
    }
    client->filter = next_filter;
    return true;
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
    if (client->file_pending || client->cgi != NULL) {
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
                                        (uintmax_t)job->remaining,
                                        "Cache-Control: public, max-age=60\r\n", client->keep_alive);
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

static void complete_cgi(int queue, struct file_pool *pool, struct http_connection *client,
                         int error_status)
{
    bool prepared = false;
    if (error_status == 0) {
        int length = cgi_write_response(client->cgi, client->response,
                                        sizeof(client->response), client->request.version,
                                        client->keep_alive);
        prepared = prepare_response(client, length, 200);
        error_status = 500;
    }
    discard_cgi(queue, client);
    if ((!prepared && !queue_response(client, pool, error_status)) ||
        !set_client_filter(queue, client, EVFILT_WRITE)) {
        close_client(queue, client, pool);
    }
}

static void complete_cgi_if_ready(int queue, struct file_pool *pool,
                                  struct http_connection *client)
{
    struct cgi_process *cgi = client->cgi;
    if (cgi->pid == 0 && (cgi->output_fd == -1 || !cgi->exited_ok)) {
        complete_cgi(queue, pool, client, 0);
    }
}

static void record_cgi_exit(struct cgi_process *cgi, int status)
{
    cgi->pid = 0;
    cgi->exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void service_cgi_output(int queue, struct file_pool *pool,
                               struct http_connection *client, const struct kevent *event)
{
    struct cgi_process *cgi = client->cgi;
    int result = event->flags & EV_ERROR ? -1 : cgi_read_output(cgi);
    if (result < 0) {
        complete_cgi(queue, pool, client, 500);
    } else if (result > 0) {
        if (cgi->pid > 0) {
            int status;
            pid_t finished;
            do {
                finished = waitpid(cgi->pid, &status, WNOHANG);
            } while (finished == -1 && errno == EINTR);
            if (finished > 0) {
                record_cgi_exit(cgi, status);
            }
        }
        complete_cgi_if_ready(queue, pool, client);
    }
}

static void reap_cgi_children(int queue, struct file_pool *pool,
                              struct http_connection clients[])
{
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid == -1 && errno == EINTR) {
            continue;
        }
        if (pid <= 0) {
            return;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct http_connection *client = &clients[i];
            if (client->cgi != NULL && client->cgi->pid == pid) {
                record_cgi_exit(client->cgi, status);
                complete_cgi_if_ready(queue, pool, client);
                break;
            }
        }
    }
}

static int run_server(int server_fd, struct file_pool *pool, const char *origin_token)
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
    struct kevent signals[3];
    EV_SET(&signals[0], SIGCHLD, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    EV_SET(&signals[1], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    EV_SET(&signals[2], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
    if (kevent(queue, signals, 3, NULL, 0, NULL) == -1) {
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

        const struct kevent *listener = NULL;
        for (int i = 0; i < count; i++) {
            const struct kevent *event = &events[i];
            if (event->filter == EVFILT_SIGNAL) {
                if (event->ident == SIGCHLD) {
                    reap_cgi_children(queue, pool, clients);
                } else {
                    exit_status = 0;
                    goto close_connections;
                }
            } else if (event->filter == EVFILT_TIMER) {
                struct http_connection *client = event->udata;
                if (event->ident >= MAX_CLIENTS) {
                    if (client->keep_alive_timer) {
                        client->keep_alive_timer = false;
                        close_client(queue, client, pool);
                    }
                } else if (client->cgi != NULL) {
                    complete_cgi(queue, pool, client, 504);
                }
            } else if (event->ident == (uintptr_t)server_fd) {
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
                if (client->cgi != NULL && event->ident == (uintptr_t)client->cgi->output_fd) {
                    service_cgi_output(queue, pool, client, event);
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

        int slot = accept_client(server_fd, clients, origin_token);
        if (slot == ACCEPT_FAILED) {
            break;
        }
        if (slot >= 0 && watch_read(queue, clients[slot].fd, &clients[slot]) == -1) {
            perror("kevent: watch client");
            close_client(queue, &clients[slot], pool);
        }
    }

close_connections:
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            close_client(queue, &clients[i], pool);
        }
    }
    close(queue);
    pid_t child;
    do {
        child = waitpid(-1, NULL, 0);
    } while (child > 0 || (child == -1 && errno == EINTR));
    return exit_status;
}

int main(int argc, char *argv[])
{
    int port = 9000;
    int option;
    while ((option = getopt(argc, argv, "p:")) == 'p') {
        char *end;
        long requested = strtol(optarg, &end, 10);
        if (*optarg < '0' || *optarg > '9' || *end != '\0' ||
            requested < 1 || requested > UINT16_MAX) {
            fputs("Port must be an integer between 1 and 65535\n", stderr);
            return 1;
        }
        port = (int)requested;
    }
    if (option != -1 || argc - optind > 1) {
        fprintf(stderr, "Usage: %s [-p port] [static-root]\n", argv[0]);
        return 1;
    }
    const char *origin_token = getenv(HTTP_ORIGIN_TOKEN_ENV);
    if (origin_token != NULL && !http_origin_token_valid(origin_token)) {
        fprintf(stderr, "%s must contain %d hexadecimal characters\n",
                HTTP_ORIGIN_TOKEN_ENV, HTTP_ORIGIN_TOKEN_LENGTH);
        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
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

    int result = run_server(server_fd, pool, origin_token);
    if (pool != NULL) {
        file_pool_destroy(pool);
        close(root_fd);
    }
    close(server_fd);
    return result;
}
