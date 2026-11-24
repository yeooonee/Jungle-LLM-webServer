#define _DARWIN_C_SOURCE
#include "proxy_response.h"

#include <inttypes.h>
#include <string.h>
#include <strings.h>

static char *next_line(char **cursor)
{
    char *line = *cursor;
    char *end = strstr(line, "\r\n");
    if (end == NULL) {
        return NULL;
    }
    *end = '\0';
    *cursor = end + 2;
    return line;
}

static bool named(const char *header, size_t length, const char *name)
{
    return strlen(name) == length && strncasecmp(header, name, length) == 0;
}

static bool valid_header_name(const char *name, const char *colon)
{
    const char *token_chars = "!#$%&'*+-.^_`|~"
                              "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    return colon != name && strspn(name, token_chars) == (size_t)(colon - name);
}

bool proxy_connection_option(const char *options, const char *name, size_t name_length)
{
    while (options != NULL && *options != '\0') {
        const char *next = strchr(options, ',');
        const char *end = next == NULL ? options + strlen(options) : next;
        while (options < end && (*options == ' ' || *options == '\t')) {
            options++;
        }
        while (end > options && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        if ((size_t)(end - options) == name_length &&
            strncasecmp(options, name, name_length) == 0) {
            return true;
        }
        options = next == NULL ? NULL : next + 1;
    }
    return false;
}

enum proxy_header_result proxy_read_headers(char *headers, size_t *header_length,
                                           const char *bytes, size_t length, size_t *consumed)
{
    *consumed = 0;
    while (*consumed < length) {
        size_t count = *header_length;
        char byte = bytes[(*consumed)++];
        char previous = count == 0 ? '\0' : headers[count - 1];
        if (count == HTTP_MAX_HEADERS || byte == '\x7f' ||
            ((unsigned char)byte < 0x20 && byte != '\t' && byte != '\r' && byte != '\n') ||
            (byte == '\n' && previous != '\r') || (previous == '\r' && byte != '\n')) {
            return PROXY_HEADERS_INVALID;
        }
        headers[count++] = byte;
        *header_length = count;
        if (count >= 4 && memcmp(headers + count - 4, "\r\n\r\n", 4) == 0) {
            headers[count] = '\0';
            return PROXY_HEADERS_READY;
        }
    }
    return PROXY_HEADERS_MORE;
}

static bool parse_length(const char *value, uintmax_t *length)
{
    value += strspn(value, " \t");
    if (*value < '0' || *value > '9') {
        return false;
    }
    uintmax_t number = 0;
    while (*value >= '0' && *value <= '9') {
        unsigned digit = (unsigned)(*value++ - '0');
        if (number > (UINTMAX_MAX - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    if (value[strspn(value, " \t")] != '\0') {
        return false;
    }
    *length = number;
    return true;
}

static bool append_header(struct proxy_response *response, const char *line)
{
    size_t length = strlen(line);
    if (length + 2 > sizeof(response->headers) - response->header_length) {
        return false;
    }
    char *output = response->headers + response->header_length;
    memcpy(output, line, length);
    memcpy(output + length, "\r\n", 2);
    response->header_length += length + 2;
    return true;
}

// Admit only explicit public responses with one numeric max-age.
static uintmax_t public_max_age(const char *value)
{
    bool public = false;
    uintmax_t max_age = 0;
    while (value != NULL) {
        value += strspn(value, " \t");
        const char *comma = strchr(value, ',');
        const char *end = comma == NULL ? value + strlen(value) : comma;
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        size_t length = (size_t)(end - value);
        if (named(value, length, "public") && !public) {
            public = true;
        } else if (length > sizeof("max-age=") - 1 && !max_age &&
                   strncasecmp(value, "max-age=", sizeof("max-age=") - 1) == 0) {
            const char *number = value + sizeof("max-age=") - 1;
            char *number_end;
            if (*number < '0' || *number > '9') {
                return 0;
            }
            max_age = strtoumax(number, &number_end, 10);
            if (number_end != end || max_age == 0 || max_age > INT32_MAX) {
                return 0;
            }
        } else {
            return 0;
        }
        value = comma == NULL ? NULL : comma + 1;
    }
    return public ? max_age : 0;
}

static void prepare_cache_headers(struct proxy_response *response, const char *cache_control,
                                  const char *date, time_t request_at)
{
    uintmax_t max_age = public_max_age(cache_control);
    struct timespec received;
    time_t now = time(NULL);
    if (max_age == 0 || now == (time_t)-1 || clock_gettime(CLOCK_MONOTONIC, &received) == -1) {
        return;
    }

    uintmax_t age = (uintmax_t)(received.tv_sec - request_at);
    if (date != NULL) {
        struct tm date_tm = {0};
        const char *end = strptime(date + strspn(date, " \t"), "%a, %d %b %Y %T GMT", &date_tm);
        if (end == NULL || end[strspn(end, " \t")] != '\0') {
            return;
        }
        time_t date_time = timegm(&date_tm);
        if (date_time == (time_t)-1) {
            return;
        }
        if (now > date_time && (uintmax_t)(now - date_time) > age) {
            age = (uintmax_t)(now - date_time);
        }
    }
    if (age >= max_age) {
        return;
    }

    if (date == NULL) {
        struct tm utc;
        char date_header[64];
        if (gmtime_r(&now, &utc) == NULL ||
            strftime(date_header, sizeof(date_header), "Date: %a, %d %b %Y %T GMT", &utc) == 0 ||
            !append_header(response, date_header)) {
            return;
        }
    }
    response->cache_received_at = received.tv_sec;
    response->cache_initial_age = age;
    response->cache_max_age = max_age;
    response->cache_header_length = response->header_length;
}

bool proxy_parse_response(char *raw_headers, bool client_keep_alive, time_t cache_request_at,
                          struct proxy_response *response)
{
    char *cursor = raw_headers;
    char *status = next_line(&cursor);
    if (status == NULL || strlen(status) < 12 ||
        (strncmp(status, "HTTP/1.1 ", 9) != 0 && strncmp(status, "HTTP/1.0 ", 9) != 0) ||
        status[9] < '2' || status[9] > '5' || status[10] < '0' || status[10] > '9' ||
        status[11] < '0' || status[11] > '9' || (status[12] != ' ' && status[12] != '\0')) {
        return false;
    }
    response->status_code = (status[9] - '0') * 100 + (status[10] - '0') * 10 + (status[11] - '0');
    bool implicit_keep_alive = status[7] == '1';
    bool no_content = response->status_code == 204;
    bool no_body = no_content || response->status_code == 304;
    bool cache_blocked = cache_request_at < 0 || response->status_code != 200;
    const char *cache_control = NULL;
    const char *date = NULL;
    const char *options = NULL;
    char *first_header = cursor;
    char *line;
    while ((line = next_line(&cursor)) != NULL && *line != '\0') {
        const char *colon = strchr(line, ':');
        if (colon == NULL || !valid_header_name(line, colon)) {
            return false;
        }
        size_t name_length = (size_t)(colon - line);
        if (named(line, name_length, "Content-Length")) {
            if (no_content || response->has_length ||
                !parse_length(colon + 1, &response->remaining)) {
                return false;
            }
            response->has_length = true;
        } else if (named(line, name_length, "Transfer-Encoding")) {
            return false;
        } else if (named(line, name_length, "Connection")) {
            if (options != NULL) {
                return false;
            }
            options = colon + 1;
        } else if (named(line, name_length, "Cache-Control")) {
            cache_blocked |= cache_control != NULL;
            cache_control = colon + 1;
        } else if (named(line, name_length, "Date")) {
            cache_blocked |= date != NULL;
            date = colon + 1;
        } else if (named(line, name_length, "Vary") || named(line, name_length, "Set-Cookie") ||
                   named(line, name_length, "Content-Range") || named(line, name_length, "Age")) {
            cache_blocked = true;
        }
    }
    if (line == NULL || proxy_connection_option(options, "Content-Length", sizeof("Content-Length") - 1)) {
        return false;
    }
    if (no_body) {
        response->has_length = true;
        response->remaining = 0;
    }
    response->client_keep_alive = response->has_length && client_keep_alive;
    response->origin_keep_alive = response->has_length &&
        !proxy_connection_option(options, "close", sizeof("close") - 1) &&
        (implicit_keep_alive || proxy_connection_option(options, "keep-alive", sizeof("keep-alive") - 1));

    if (!append_header(response, status)) {
        return false;
    }
    for (line = first_header; *line != '\0'; line += strlen(line) + 2) {
        size_t name_length = (size_t)(strchr(line, ':') - line);
        if (named(line, name_length, "Connection") || named(line, name_length, "Keep-Alive") ||
            named(line, name_length, "Proxy-Connection") ||
            named(line, name_length, "TE") || named(line, name_length, "Trailer") ||
            named(line, name_length, "Upgrade") ||
            proxy_connection_option(options, line, name_length)) {
            continue;
        }
        if (!append_header(response, line)) {
            return false;
        }
    }
    if (!cache_blocked && response->has_length &&
        !proxy_connection_option(options, "cache-control", sizeof("cache-control") - 1) &&
        !proxy_connection_option(options, "date", sizeof("date") - 1)) {
        prepare_cache_headers(response, cache_control, date, cache_request_at);
    }
    const char *connection = response->client_keep_alive ? "Connection: keep-alive"
                                                        : "Connection: close";
    return append_header(response, connection) && append_header(response, "");
}
