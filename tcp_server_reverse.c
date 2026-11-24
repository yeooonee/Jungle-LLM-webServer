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
#include <unistd.h>

enum {
    LISTEN_PORT = 8082,
    MAX_CLIENTS = 64,
    MAX_EVENTS = MAX_CLIENTS * 2 + 3,
    BUFFER_SIZE = 4096,
    KEEP_ALIVE_TIMEOUT_MS = 5000
};
#define HELLO_ETAG "\"hello-v1\""

struct http_connection {
    int fd;
    int slot;
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
    const char *body = "";
    const char *extra_headers = "";
    if (status == 200 && strcmp(state->request.method, "GET") == 0) {
        const char *path = request_path(state->request.uri);
        if (request_path_is(path, "/redirect/permanent")) {
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

static bool process_client_input(struct http_connection *state)
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
        state->keep_alive = !state->request.connection_close &&
                            (strcmp(state->request.version, "HTTP/1.1") == 0 ||
                             state->request.connection_keep_alive);
    }
    return queue_response(state, status);
}

static void cancel_keep_alive_timer(int queue, struct http_connection *state)
{
    if (!state->keep_alive_timer) {
        return;
    }
    struct kevent change;
    EV_SET(&change, (uintptr_t)state->slot, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == -1 && errno != ENOENT) {
        perror("kevent: remove keep-alive timer");
    }
    state->keep_alive_timer = false;
}

static bool finish_response(int queue, struct http_connection *state)
{
    if (!state->keep_alive) {
        return false;
    }
    reset_request(state);
    if (state->input_length > 0) {
        return process_client_input(state);
    }

    struct kevent change;
    EV_SET(&change, (uintptr_t)state->slot, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
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

static bool handle_client(int queue, struct http_connection *state, bool hangup)
{
    if (state->response_length == 0) {
        ssize_t received = recv(state->fd, state->input + state->input_length,
                                sizeof(state->input) - state->input_length, 0);
        if (received > 0) {
            cancel_keep_alive_timer(queue, state);
            state->input_length += (size_t)received;
            return process_client_input(state);
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
        return true;
    }
    if (state->interim_response) {
        // After 100 Continue, resume reading this request's body.
        state->response_length = 0;
        state->response_sent = 0;
        state->interim_response = false;
        return true;
    }
    return finish_response(queue, state);
}

static void close_client(int queue, struct http_connection *client)
{
    cancel_keep_alive_timer(queue, client);
    reset_request(client);
    close(client->fd);
    client->fd = -1;
}

enum { ACCEPT_RETRY = -1, ACCEPT_FAILED = -2 };

static int accept_client(int server_fd, struct http_connection clients[])
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
    if (!set_client_filter(queue, client, next_filter)) {
        close_client(queue, client);
    }
}

static int run_server(int server_fd)
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

        const struct kevent *listener = NULL;
        for (int i = 0; i < count; i++) {
            const struct kevent *event = &events[i];
            if (event->filter == EVFILT_SIGNAL) {
                exit_status = 0;
                goto close_connections;
            } else if (event->filter == EVFILT_TIMER) {
                struct http_connection *client = event->udata;
                if (client->keep_alive_timer) {
                    client->keep_alive_timer = false;
                    close_client(queue, client);
                }
            } else if (event->ident == (uintptr_t)server_fd) {
                listener = event;
            } else {
                service_kqueue_client(queue, event);
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

        int slot = accept_client(server_fd, clients);
        if (slot == ACCEPT_FAILED) {
            break;
        }
        if (slot >= 0 && watch_read(queue, clients[slot].fd, &clients[slot]) == -1) {
            perror("kevent: watch client");
            close_client(queue, &clients[slot]);
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
    if (option != -1 || optind != argc) {
        fprintf(stderr, "Usage: %s [-p port]\n", argv[0]);
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
    if (listen(server_fd, 8) == -1) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("HTTP server listening on http://127.0.0.1:%d\n", port);

    int result = run_server(server_fd);
    close(server_fd);
    return result;
}
