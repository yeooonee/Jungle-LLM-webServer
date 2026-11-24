#ifndef PROXY_RESPONSE_H
#define PROXY_RESPONSE_H

#include "http.h"

#include <time.h>

struct proxy_response {
    bool has_length;
    bool origin_keep_alive;
    bool client_keep_alive;
    uintmax_t remaining;
    time_t cache_received_at;
    uintmax_t cache_initial_age;
    uintmax_t cache_max_age;
    size_t cache_header_length;
    size_t header_length;
    char headers[HTTP_MAX_HEADERS + 64];
};

enum proxy_header_result { PROXY_HEADERS_MORE, PROXY_HEADERS_READY, PROXY_HEADERS_INVALID };

enum proxy_header_result proxy_read_headers(char *headers, size_t *header_length,
                                           const char *bytes, size_t length, size_t *consumed);
bool proxy_connection_option(const char *options, const char *name, size_t name_length);
bool proxy_parse_response(char *raw_headers, bool client_keep_alive, time_t cache_request_at,
                          struct proxy_response *response);

#endif
