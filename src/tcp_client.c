#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { SERVER_PORT = 9000 };

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int wait_for_socket(int fd, short events)
{
    struct pollfd socket = {.fd = fd, .events = events};
    int result;
    do {
        result = poll(&socket, 1, -1);
    } while (result == -1 && errno == EINTR);

    return result == -1 ? -1 : socket.revents;
}

static int connect_to_server(int fd, const struct sockaddr_in *address)
{
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0) {
        return 0;
    }
    if (errno != EINPROGRESS && errno != EINTR) {
        return -1;
    }

    if (wait_for_socket(fd, POLLOUT) == -1) {
        return -1;
    }

    // A completed connection attempt may have succeeded or failed.
    int error;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1) {
        return -1;
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static int send_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t count = send(fd, data, length, 0);
        if (count > 0) {
            data += count;
            length -= (size_t)count;
            continue;
        }
        if (count == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }

        int events = wait_for_socket(fd, POLLOUT);
        if (events == -1) {
            return -1;
        }
        if (!(events & POLLOUT)) {
            errno = EPIPE;
            return -1;
        }
    }
    return 0;
}

static int print_response(int fd)
{
    char incoming[4096];
    bool received_response = false;

    for (;;) {
        ssize_t count = recv(fd, incoming, sizeof(incoming), 0);
        if (count > 0) {
            received_response = true;
            if (fwrite(incoming, 1, (size_t)count, stdout) != (size_t)count) {
                return -1;
            }
            continue;
        }
        if (count == 0) {
            if (!received_response) {
                errno = ECONNRESET;
                return -1;
            }
            return fflush(stdout);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        if (wait_for_socket(fd, POLLIN) == -1) {
            return -1;
        }
    }
}

int main(int argc, char *argv[])
{
    bool get = argc == 3 && strcmp(argv[1], "GET") == 0;
    bool post = argc == 4 && strcmp(argv[1], "POST") == 0;
    if ((!get && !post) || argv[2][0] != '/' || strpbrk(argv[2], " \t\r\n")) {
        fprintf(stderr, "Usage: %s GET /path\n       %s POST /path 'body'\n",
                argv[0], argv[0]);
        return 1;
    }

    const char *body = post ? argv[3] : "";
    size_t body_length = strlen(body);
    if (body_length > HTTP_MAX_BODY) {
        fputs("Request body is too large\n", stderr);
        return 1;
    }

    char content_headers[96] = "";
    if (post) {
        snprintf(content_headers, sizeof(content_headers),
                 "Content-Type: text/plain\r\nContent-Length: %zu\r\n", body_length);
    }
    char headers[HTTP_MAX_HEADERS + 1];
    int header_length = snprintf(headers, sizeof(headers),
        "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n%s\r\n",
        argv[1], argv[2], SERVER_PORT, content_headers);
    if (header_length < 0 || (size_t)header_length >= sizeof(headers)) {
        fputs("Request headers are too large\n", stderr);
        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("socket");
        return 1;
    }
    if (set_nonblocking(client_fd) == -1) {
        perror("fcntl");
        close(client_fd);
        return 1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        fputs("Invalid IPv4 address\n", stderr);
        close(client_fd);
        return 1;
    }

    if (connect_to_server(client_fd, &address) == -1) {
        perror("connect");
        close(client_fd);
        return 1;
    }

    if (send_all(client_fd, headers, (size_t)header_length) == -1 ||
        send_all(client_fd, body, body_length) == -1) {
        perror("send request");
        close(client_fd);
        return 1;
    }

    int result = print_response(client_fd);
    if (result == -1) {
        perror("receive response");
    }
    close(client_fd);
    return result == -1 ? 1 : 0;
}
