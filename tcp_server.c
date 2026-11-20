#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { MAX_CLIENTS = 64, BUFFER_SIZE = 4096 };

struct http_connection {
    char headers[HTTP_MAX_HEADERS + 1];
    size_t header_length;
    bool headers_complete;
    struct http_request request;
    size_t body_received;
    char response[HTTP_RESPONSE_SIZE];
    size_t response_length;
    size_t response_sent;
};

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Return 0 while more bytes are needed, or an HTTP status when ready to reply.
static int read_request_bytes(struct http_connection *state, const char *data,
                              size_t length)
{
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
            int status = http_parse_headers(state->headers, &state->request);
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

    return state->body_received == state->request.content_length ? 200 : 0;
}

static bool queue_response(struct pollfd *client, struct http_connection *state,
                           int status)
{
    const char *body = "";
    if (status == 200 && strcmp(state->request.method, "GET") == 0) {
        body = "hello";
    }
    int length = http_write_response(state->response, sizeof(state->response),
                                     state->request.version, status, body);
    if (length < 0 || (size_t)length >= sizeof(state->response)) {
        return false;
    }
    state->response_length = (size_t)length;
    client->events = POLLOUT;
    return true;
}

static bool handle_client(struct pollfd *client, struct http_connection *state)
{
    if (client->revents & (POLLERR | POLLNVAL)) {
        return false;
    }

    if (client->events == POLLIN) {
        char data[BUFFER_SIZE];
        ssize_t received = recv(client->fd, data, sizeof(data), 0);
        if (received > 0) {
            int status = read_request_bytes(state, data, (size_t)received);
            return status == 0 || queue_response(client, state, status);
        }
        if (received == 0) {
            return state->header_length > 0 && queue_response(client, state, 400);
        }
    } else {
        ssize_t sent = send(client->fd, state->response + state->response_sent,
                            state->response_length - state->response_sent, 0);
        if (sent > 0) {
            state->response_sent += (size_t)sent;
            return state->response_sent < state->response_length;
        }
        if (sent == 0) {
            return false;
        }
    }

    return errno == EINTR ||
           ((errno == EAGAIN || errno == EWOULDBLOCK) &&
            !(client->revents & POLLHUP));
}

static int run_server(int server_fd)
{
    struct pollfd fds[MAX_CLIENTS + 1] = {{.fd = server_fd, .events = POLLIN}};
    struct http_connection clients[MAX_CLIENTS];

    for (int i = 1; i <= MAX_CLIENTS; i++) {
        fds[i].fd = -1;  // poll() ignores unused slots.
    }

    for (;;) {
        if (poll(fds, MAX_CLIENTS + 1, -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        for (int i = 1; i <= MAX_CLIENTS; i++) {
            if (fds[i].revents && !handle_client(&fds[i], &clients[i - 1])) {
                free(clients[i - 1].request.body);
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fputs("Listening socket failed\n", stderr);
            break;
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            break;
        }

        int slot = 1;
        while (slot <= MAX_CLIENTS && fds[slot].fd != -1) {
            slot++;
        }
        if (slot > MAX_CLIENTS) {
            close(client_fd);
            continue;
        }
        if (set_nonblocking(client_fd) == -1) {
            perror("fcntl");
            close(client_fd);
            continue;
        }

        clients[slot - 1] = (struct http_connection){0};
        fds[slot] = (struct pollfd){.fd = client_fd, .events = POLLIN};
    }

    for (int i = 1; i <= MAX_CLIENTS; i++) {
        if (fds[i].fd != -1) {
            free(clients[i - 1].request.body);
            close(fds[i].fd);
        }
    }
    return 1;
}

int main(void)
{
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return 1;
    }
    if (set_nonblocking(server_fd) == -1) {
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
    address.sin_port = htons(8080);
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

    puts("HTTP server listening on http://127.0.0.1:8080");

    int result = run_server(server_fd);
    close(server_fd);
    return result;
}
