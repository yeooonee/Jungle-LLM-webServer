#ifndef PROXY_CACHE_H
#define PROXY_CACHE_H

#include "proxy_response.h"

struct proxy_cache_entry;

struct proxy_cache_hit {
    struct proxy_cache_entry *entry;
    const char *headers;
    size_t header_length;
    const char *body;
    size_t body_length;
    uintmax_t age;
};

time_t proxy_cache_now(void);
bool proxy_cache_lookup(const char *uri, time_t now, struct proxy_cache_hit *hit);
bool proxy_cache_note_request(const char *uri, time_t now);
struct proxy_cache_entry *proxy_cache_begin(const char *uri, const struct proxy_response *response);
void proxy_cache_append(struct proxy_cache_entry **entry, const char *bytes, size_t length);
void proxy_cache_release(struct proxy_cache_entry *entry);
void proxy_cache_destroy(void);

#endif
