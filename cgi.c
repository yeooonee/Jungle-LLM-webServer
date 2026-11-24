#include "cgi.h"
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cgi_process *cgi_start_sum(const char *query, const char *version, unsigned int server_port)
{
    char query_string[HTTP_MAX_HEADERS + sizeof("QUERY_STRING=")];
    char protocol[32];
    char port[sizeof("SERVER_PORT=65535")];
    int query_length = snprintf(query_string, sizeof(query_string), "QUERY_STRING=%s", query);
    int protocol_length = snprintf(protocol, sizeof(protocol), "SERVER_PROTOCOL=%s", version);
    int port_length = snprintf(port, sizeof(port), "SERVER_PORT=%u", server_port);
    if (query_length < 0 || (size_t)query_length >= sizeof(query_string) ||
        protocol_length < 0 || (size_t)protocol_length >= sizeof(protocol) ||
        port_length < 0 || (size_t)port_length >= sizeof(port)) {
        return NULL;
    }
    char *arguments[] = {"./cgi_sum", NULL};
    char *environment[] = {
        "GATEWAY_INTERFACE=CGI/1.1", "REQUEST_METHOD=GET", "SCRIPT_NAME=/cgi-bin/sum",
        "SERVER_NAME=127.0.0.1", "SERVER_SOFTWARE=Jungle-LLM-webServer",
        protocol, port, query_string, NULL,
    };

    struct cgi_process *cgi = calloc(1, sizeof(*cgi));
    if (cgi == NULL) {
        return NULL;
    }
    int output[2];
    if (pipe(output) == -1) {
        free(cgi);
        return NULL;
    }
    int input_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input_fd == -1 || fcntl(output[0], F_SETFL, O_NONBLOCK) == -1 ||
        fcntl(output[0], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(output[1], F_SETFD, FD_CLOEXEC) == -1) {
        if (input_fd != -1) {
            close(input_fd);
        }
        close(output[0]);
        close(output[1]);
        free(cgi);
        return NULL;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // The server has worker threads. The child uses async-signal-safe calls until exec.
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) == -1 ||
            dup2(input_fd, STDIN_FILENO) == -1 ||
            fcntl(STDOUT_FILENO, F_SETFD, 0) == -1 ||
            fcntl(STDIN_FILENO, F_SETFD, 0) == -1) {
            _exit(127);
        }
        execve(arguments[0], arguments, environment);
        _exit(127);
    }

    close(input_fd);
    close(output[1]);
    if (pid == -1) {
        close(output[0]);
        free(cgi);
        return NULL;
    }
    cgi->pid = pid;
    cgi->output_fd = output[0];
    return cgi;
}

int cgi_read_output(struct cgi_process *cgi)
{
    for (;;) {
        ssize_t count = read(cgi->output_fd, cgi->output + cgi->output_length,
                             sizeof(cgi->output) - cgi->output_length);
        if (count > 0) {
            cgi->output_length += (size_t)count;
            if (cgi->output_length > CGI_OUTPUT_LIMIT) {
                return -1;
            }
        } else if (count == 0) {
            close(cgi->output_fd);
            cgi->output_fd = -1;
            return 1;
        } else if (errno != EINTR) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
        }
    }
}

int cgi_write_response(const struct cgi_process *cgi, char *response, size_t capacity,
                       const char *version, bool keep_alive)
{
    static const char bad_request[] = "Status: 400 Bad Request\r\n";
    static const char json_header[] = "Content-Type: application/json\r\n\r\n";
    if (!cgi->exited_ok || memchr(cgi->output, '\0', cgi->output_length) != NULL) {
        return -1;
    }
    const char *output = cgi->output;
    int status = 200;
    if (strncmp(output, bad_request, sizeof(bad_request) - 1) == 0) {
        status = 400;
        output += sizeof(bad_request) - 1;
    }
    if (strncmp(output, json_header, sizeof(json_header) - 1) != 0) {
        return -1;
    }
    return http_write_json_response(response, capacity, version, status,
                                    output + sizeof(json_header) - 1, keep_alive);
}

void cgi_dispose(struct cgi_process *cgi)
{
    if (cgi->output_fd != -1) {
        close(cgi->output_fd);
    }
    if (cgi->pid > 0) {
        kill(cgi->pid, SIGKILL);
    }
    free(cgi);
}
