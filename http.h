#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HTTP_MAX_HEADERS = 8192,
    HTTP_MAX_BODY = 65536,
    HTTP_RESPONSE_SIZE = 1024
};

struct http_request {
    const char *method;
    const char *uri;
    const char *version;
    const char *host;
    size_t content_length;
    bool connection_close;
    bool connection_keep_alive;
    bool expect_continue;
    bool etag_matches;
    bool bypass_cache;
    unsigned char *body;
};

// The parsed strings point into the mutable, NUL-terminated headers buffer.
int http_parse_headers(char *headers, struct http_request *request,
                       const char *etag);
int http_write_response(char *response, size_t capacity, const char *version,
                        int status, const char *body, const char *extra_headers,
                        bool keep_alive);
int http_write_json_response(char *response, size_t capacity, const char *version,
                             int status, const char *body, bool keep_alive);
int http_write_file_headers(char *response, size_t capacity, const char *version,
                            const char *content_type, uintmax_t file_size,
                            const char *extra_headers, bool keep_alive);

#endif
