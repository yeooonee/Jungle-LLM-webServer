#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE // Expose O_NOFOLLOW in the macOS SDK.
#endif

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
#include <strings.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_CLIENTS = 64, BUFFER_SIZE = 4096 };
#define HELLO_ETAG "\"hello-v1\""
#define STATIC_PREFIX "/static/"

struct http_connection {
    int fd;
    char headers[HTTP_MAX_HEADERS + 1];
    size_t header_length;
    bool headers_complete;
    struct http_request request;
    size_t body_received;
    char response[HTTP_RESPONSE_SIZE];
    size_t response_length;
    size_t response_sent;
    bool interim_response;
    int file_fd;
    off_t file_remaining;
    char file_buffer[BUFFER_SIZE];
    size_t file_length;
    size_t file_sent;
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

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    return -1;
}

static int decode_path_component(const char *path, size_t length,
                                 char *name)
{
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)path[i];
        if (byte == '%') {
            if (length - i < 3) {
                return 400;
            }
            int high = hex_digit((unsigned char)path[i + 1]);
            int low = hex_digit((unsigned char)path[i + 2]);
            if (high < 0 || low < 0) {
                return 400;
            }
            byte = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (byte == '/' || byte == '\\' || byte < 0x20 || byte == 0x7f) {
            return 403;
        }
        name[used++] = (char)byte;
    }
    name[used] = '\0';

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 403;
    }
    return used == 0 ? 404 : 200;
}

static int file_open_status(int directory, const char *name, int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    case ENOTDIR: {
        struct stat info;
        // O_DIRECTORY may report ENOTDIR for a symlink on some filesystems.
        if (fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(info.st_mode)) {
            return 403;
        }
        return 404;
    }
    case ENOENT:
    case ENAMETOOLONG:
        return 404;
    default:
        return 500;
    }
}

static const char *file_content_type(const char *name)
{
    static const struct {
        const char *extension;
        const char *content_type;
    } types[] = {
        {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
        {".js", "text/javascript"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".svg", "image/svg+xml"},
        {".webp", "image/webp"}, {".ico", "image/x-icon"}, {".txt", "text/plain"},
        {".json", "application/json"}, {".pdf", "application/pdf"},
    };
    const char *extension = strrchr(name, '.');
    if (extension != NULL) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strcasecmp(extension, types[i].extension) == 0) {
                return types[i].content_type;
            }
        }
    }
    return "application/octet-stream";
}

static int open_static_file(struct http_connection *state, int root_fd,
                            const char *path, const char **content_type)
{
    if (root_fd == -1) {
        return 404;
    }

    int directory = root_fd;
    // The request headers already bound the length of each path component.
    char name[HTTP_MAX_HEADERS + 1];
    int status;
    for (;;) {
        size_t length = strcspn(path, "/?");
        status = decode_path_component(path, length, name);
        if (status != 200) {
            break;
        }

        bool last = path[length] != '/';
        int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
        if (!last) {
            flags |= O_DIRECTORY;
        }
        int fd = openat(directory, name, flags);
        if (fd == -1) {
            status = file_open_status(directory, name, errno);
            break;
        }
        if (last) {
            struct stat info;
            if (fstat(fd, &info) == -1 || info.st_size < 0) {
                status = 500;
            } else if (!S_ISREG(info.st_mode)) {
                status = 403;
            }
            if (status == 200) {
                state->file_fd = fd;
                state->file_remaining = info.st_size;
                *content_type = file_content_type(name);
            } else {
                close(fd);
            }
            break;
        }
        if (directory != root_fd) {
            close(directory);
        }
        directory = fd;
        path += length + 1;
    }
    if (directory != root_fd) {
        close(directory);
    }
    return status;
}

static bool queue_response(struct http_connection *state, int root_fd, int status)
{
    const char *body = "";
    const char *extra_headers = "";
    const char *content_type = NULL;
    if (status == 200 && strcmp(state->request.method, "GET") == 0) {
        const char *path = request_path(state->request.uri);
        size_t prefix_length = sizeof(STATIC_PREFIX) - 1;
        if (strncmp(path, STATIC_PREFIX, prefix_length) == 0) {
            status = open_static_file(state, root_fd, path + prefix_length, &content_type);
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
    int length;
    if (state->file_fd != -1) {
        length = http_write_file_headers(state->response, sizeof(state->response),
                                         state->request.version, content_type,
                                         (uintmax_t)state->file_remaining);
    } else {
        length = http_write_response(state->response, sizeof(state->response),
                                     state->request.version, status, body, extra_headers);
    }
    if (length < 0 || (size_t)length >= sizeof(state->response)) {
        return false;
    }
    state->response_length = (size_t)length;
    state->response_sent = 0;
    state->interim_response = status == 100;
    return true;
}

static bool can_retry_socket_io(bool hangup)
{
    return errno == EINTR ||
           ((errno == EAGAIN || errno == EWOULDBLOCK) && !hangup);
}

static bool send_file_chunk(struct http_connection *state, bool hangup)
{
    if (state->file_sent == state->file_length) {
        size_t length = sizeof(state->file_buffer);
        if (state->file_remaining < (off_t)length) {
            length = (size_t)state->file_remaining;
        }
        ssize_t count = read(state->file_fd, state->file_buffer, length);
        if (count <= 0) {
            return count < 0 && errno == EINTR;
        }
        state->file_length = (size_t)count;
        state->file_sent = 0;
    }

    ssize_t sent = send(state->fd, state->file_buffer + state->file_sent,
                        state->file_length - state->file_sent, 0);
    if (sent <= 0) {
        return sent < 0 && can_retry_socket_io(hangup);
    }
    state->file_sent += (size_t)sent;
    state->file_remaining -= (off_t)sent;
    return state->file_remaining > 0;
}

static bool handle_client(struct http_connection *state, int root_fd, bool hangup)
{
    if (state->response_length == 0) {
        char data[BUFFER_SIZE];
        ssize_t received = recv(state->fd, data, sizeof(data), 0);
        if (received > 0) {
            int status = read_request_bytes(state, data, (size_t)received);
            return status == 0 || queue_response(state, root_fd, status);
        }
        if (received == 0) {
            return state->header_length > 0 && queue_response(state, root_fd, 400);
        }
        return can_retry_socket_io(hangup);
    }

    if (state->response_sent == state->response_length) {
        return send_file_chunk(state, hangup);
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
    return state->file_remaining > 0;
}

static void close_client(struct http_connection *client)
{
    free(client->request.body);
    if (client->file_fd != -1) {
        close(client->file_fd);
    }
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
    if (set_nonblocking(fd) == -1) {
        perror("fcntl");
        close(fd);
        return ACCEPT_RETRY;
    }

    clients[slot] = (struct http_connection){.fd = fd, .file_fd = -1};
    return slot;
}

static int watch_read(int queue, int fd, struct http_connection *client)
{
    struct kevent change;
    EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_ADD, 0, 0, client);
    return kevent(queue, &change, 1, NULL, 0, NULL);
}

static void service_kqueue_client(int queue, int root_fd, const struct kevent *event)
{
    struct http_connection *client = event->udata;
    bool hangup = (event->flags & EV_EOF) != 0;
    if ((event->flags & EV_ERROR) || !handle_client(client, root_fd, hangup)) {
        close_client(client);
        return;
    }

    short next_filter = client->response_length > 0 ? EVFILT_WRITE : EVFILT_READ;
    if (event->filter != next_filter) {
        struct kevent changes[2];
        EV_SET(&changes[0], event->ident, event->filter, EV_DELETE, 0, 0, NULL);
        EV_SET(&changes[1], event->ident, next_filter, EV_ADD, 0, 0, client);
        if (kevent(queue, changes, 2, NULL, 0, NULL) == -1) {
            perror("kevent: change client filter");
            close_client(client);
        }
    }
}

static int run_server(int server_fd, int root_fd)
{
    int queue = kqueue();
    if (queue == -1) {
        perror("kqueue");
        return 1;
    }
    if (watch_read(queue, server_fd, NULL) == -1) {
        perror("kevent: watch server");
        close(queue);
        return 1;
    }

    struct http_connection clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
    }

    struct kevent events[MAX_CLIENTS + 1];
    for (;;) {
        int count = kevent(queue, NULL, 0, events, MAX_CLIENTS + 1, NULL);
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("kevent");
            break;
        }

        const struct kevent *listener = NULL;
        for (int i = 0; i < count; i++) {
            if (events[i].udata == NULL) {
                listener = &events[i];
            } else {
                service_kqueue_client(queue, root_fd, &events[i]);
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
            close_client(&clients[slot]);
        }
    }
    close(queue);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            close_client(&clients[i]);
        }
    }
    return 1;
}

int main(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [static-root]\n", argv[0]);
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

    int root_fd = -1;
    if (argc == 2) {
        root_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (root_fd == -1) {
            perror(argv[1]);
            close(server_fd);
            return 1;
        }
    }

    puts("HTTP server listening on http://127.0.0.1:8080");

    int result = run_server(server_fd, root_fd);
    if (root_fd != -1) {
        close(root_fd);
    }
    close(server_fd);
    return result;
}
