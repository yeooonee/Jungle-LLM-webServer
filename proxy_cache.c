#include "proxy_cache.h"

#include <stdlib.h>
#include <string.h>

enum {
    MAX_ENTRIES = 16,
    MAX_RECENT_URLS = 64,
    ADMISSION_WINDOW_SECONDS = 30,
    MAX_BODY_BYTES = 2 * 1024 * 1024,
    CACHE_BYTES = 8 * 1024 * 1024,
};

struct proxy_cache_entry {
    size_t references;
    size_t allocation_size;
    size_t uri_length;
    size_t header_length;
    size_t body_length;
    size_t body_received;
    time_t received_at;
    uintmax_t initial_age;
    uintmax_t max_age;
    uint64_t last_used;
    char data[];
};

struct recent_request {
    char uri[HTTP_MAX_HEADERS];
    time_t last_seen;
    uint64_t last_used;
};

// Only the forward proxy's event loop reads or mutates this state.
static struct proxy_cache_entry *entries[MAX_ENTRIES];
static struct recent_request recent[MAX_RECENT_URLS];
static size_t allocated_bytes;
static uint64_t use_sequence;

time_t proxy_cache_now(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ? now.tv_sec : (time_t)-1;
}

static uintmax_t entry_age(const struct proxy_cache_entry *entry, time_t now)
{
    return entry->initial_age + (uintmax_t)(now - entry->received_at);
}

void proxy_cache_release(struct proxy_cache_entry *entry)
{
    if (entry != NULL && --entry->references == 0) {
        allocated_bytes -= entry->allocation_size;
        free(entry);
    }
}

static void remove_entry(size_t slot)
{
    proxy_cache_release(entries[slot]);
    entries[slot] = NULL;
}

static void remove_expired(time_t now)
{
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        if (entries[i] != NULL && entry_age(entries[i], now) >= entries[i]->max_age) {
            remove_entry(i);
        }
    }
}

bool proxy_cache_lookup(const char *uri, time_t now, struct proxy_cache_hit *hit)
{
    remove_expired(now);
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        struct proxy_cache_entry *entry = entries[i];
        if (entry == NULL || strcmp(entry->data, uri) != 0) {
            continue;
        }
        entry->last_used = ++use_sequence;
        entry->references++;
        *hit = (struct proxy_cache_hit){
            .entry = entry,
            .headers = entry->data + entry->uri_length,
            .header_length = entry->header_length,
            .body = entry->data + entry->uri_length + entry->header_length,
            .body_length = entry->body_length,
            .age = entry_age(entry, now),
        };
        return true;
    }
    return false;
}

bool proxy_cache_note_request(const char *uri, time_t now)
{
    if (strlen(uri) >= sizeof(recent[0].uri)) {
        return false;
    }
    struct recent_request *oldest = &recent[0];
    for (size_t i = 0; i < MAX_RECENT_URLS; i++) {
        struct recent_request *request = &recent[i];
        if (strcmp(request->uri, uri) == 0) {
            bool repeated = now - request->last_seen <= ADMISSION_WINDOW_SECONDS;
            request->last_seen = now;
            request->last_used = ++use_sequence;
            return repeated;
        }
        if (request->last_used < oldest->last_used) {
            oldest = request;
        }
    }
    strcpy(oldest->uri, uri);
    oldest->last_seen = now;
    oldest->last_used = ++use_sequence;
    return false;
}

static size_t oldest_entry(bool unpinned_only)
{
    size_t oldest = MAX_ENTRIES;
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        const struct proxy_cache_entry *entry = entries[i];
        if (entry != NULL && (!unpinned_only || entry->references == 1) &&
            (oldest == MAX_ENTRIES || entry->last_used < entries[oldest]->last_used)) {
            oldest = i;
        }
    }
    return oldest;
}

struct proxy_cache_entry *proxy_cache_begin(const char *uri, const struct proxy_response *response)
{
    if (response->cache_max_age == 0 || response->remaining > MAX_BODY_BYTES) {
        return NULL;
    }
    size_t uri_length = strlen(uri) + 1;
    size_t body_length = (size_t)response->remaining;
    size_t bytes = sizeof(struct proxy_cache_entry) + uri_length +
                   response->cache_header_length + body_length;

    remove_expired(response->cache_received_at);
    while (bytes > CACHE_BYTES - allocated_bytes) {
        size_t oldest = oldest_entry(true);
        if (oldest == MAX_ENTRIES) {
            return NULL;
        }
        remove_entry(oldest);
    }

    struct proxy_cache_entry *entry = malloc(bytes);
    if (entry == NULL) {
        return NULL;
    }
    allocated_bytes += bytes;
    *entry = (struct proxy_cache_entry){
        .references = 1,
        .allocation_size = bytes,
        .uri_length = uri_length,
        .header_length = response->cache_header_length,
        .body_length = body_length,
        .received_at = response->cache_received_at,
        .initial_age = response->cache_initial_age,
        .max_age = response->cache_max_age,
    };
    memcpy(entry->data, uri, uri_length);
    memcpy(entry->data + uri_length, response->headers, entry->header_length);
    return entry;
}

static void publish(struct proxy_cache_entry *entry)
{
    time_t now = proxy_cache_now();
    if (now < 0 || entry_age(entry, now) >= entry->max_age) {
        proxy_cache_release(entry);
        return;
    }
    remove_expired(now);
    size_t slot = MAX_ENTRIES;
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        if (entries[i] == NULL) {
            slot = i;
        } else if (strcmp(entries[i]->data, entry->data) == 0) {
            // A concurrent fetch already published a fresh response for this URL.
            proxy_cache_release(entry);
            return;
        }
    }
    if (slot == MAX_ENTRIES) {
        slot = oldest_entry(false);
        remove_entry(slot);
    }
    entry->last_used = ++use_sequence;
    entries[slot] = entry;
}

void proxy_cache_append(struct proxy_cache_entry **capture, const char *bytes, size_t length)
{
    struct proxy_cache_entry *entry = *capture;
    if (entry == NULL) {
        return;
    }
    if (length > entry->body_length - entry->body_received) {
        *capture = NULL;
        proxy_cache_release(entry);
        return;
    }
    char *body = entry->data + entry->uri_length + entry->header_length;
    memcpy(body + entry->body_received, bytes, length);
    entry->body_received += length;
    if (entry->body_received == entry->body_length) {
        *capture = NULL;
        publish(entry);
    }
}

void proxy_cache_destroy(void)
{
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        remove_entry(i);
    }
}
