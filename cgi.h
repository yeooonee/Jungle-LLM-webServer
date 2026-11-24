#ifndef CGI_H
#define CGI_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum { CGI_OUTPUT_LIMIT = 512, CGI_TIMEOUT_MS = 5000 };

struct cgi_process {
    pid_t pid;
    int output_fd;
    bool exited_ok;
    size_t output_length;
    char output[CGI_OUTPUT_LIMIT + 1];
};

struct cgi_process *cgi_start_sum(const char *query, const char *version, unsigned int server_port);
// Returns 1 after EOF, 0 while waiting for more output, or -1 on error.
int cgi_read_output(struct cgi_process *cgi);
int cgi_write_response(const struct cgi_process *cgi, char *response, size_t capacity,
                       const char *version, bool keep_alive);
void cgi_dispose(struct cgi_process *cgi);

#endif
