#include "http.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool is_digit(unsigned char ch)
{
    return ch >= '0' && ch <= '9';
}

static bool is_token(const char *value)
{
    if (*value == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char ch = *p;
        if (!is_digit(ch) && !(ch >= 'A' && ch <= 'Z') &&
            !(ch >= 'a' && ch <= 'z') && !strchr("!#$%&'*+-.^_`|~", ch)) {
            return false;
        }
    }
    return true;
}

static char *next_line(char **remaining)
{
    char *line = *remaining;
    char *end = strstr(line, "\r\n");
    if (end == NULL) {
        return NULL;
    }
    *end = '\0';
    *remaining = end + 2;
    return line;
}

static int parse_request_line(char *line, struct http_request *request)
{
    char *uri = strchr(line, ' ');
    if (uri == NULL) {
        return 400;
    }
    *uri++ = '\0';
    char *version = strchr(uri, ' ');
    if (version == NULL) {
        return 400;
    }
    *version++ = '\0';

    if (!is_token(line) || *uri == '\0') {
        return 400;
    }
    for (const unsigned char *p = (const unsigned char *)uri; *p; p++) {
        if (*p < 0x21 || *p > 0x7e || *p == '#' || *p == '\\') {
            return 400;
        }
    }

    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 ||
        !is_digit((unsigned char)version[5]) || version[6] != '.' ||
        !is_digit((unsigned char)version[7])) {
        return 400;
    }
    if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) {
        return 505;
    }

    request->method = line;
    request->uri = uri;
    request->version = version;
    return 200;
}

static char *trim_whitespace(char *value)
{
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return value;
}

static bool is_header_value(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p < 0x20 && *p != '\t') || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool is_host_value(const char *value)
{
    if (*value == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x21 || *p > 0x7e || strchr(",/@\\?#", *p)) {
            return false;
        }
    }
    return true;
}

static int parse_content_length(const char *value, size_t *length)
{
    size_t result = 0;
    if (*value == '\0') {
        return 400;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!is_digit(*p)) {
            return 400;
        }
        // Stop accumulating after crossing the body limit to avoid overflow.
        if (result <= HTTP_MAX_BODY) {
            result = result * 10 + (*p - '0');
        }
    }
    if (result > HTTP_MAX_BODY) {
        return 413;
    }
    *length = result;
    return 200;
}

static bool parse_connection(char *value, struct http_request *request)
{
    while (value != NULL) {
        char *comma = strchr(value, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        value = trim_whitespace(value);
        if (*value && !is_token(value)) {
            return false;
        }
        if (strcasecmp(value, "close") == 0) {
            request->connection_close = true;
        } else if (strcasecmp(value, "keep-alive") == 0) {
            request->connection_keep_alive = true;
        }
        value = comma == NULL ? NULL : comma + 1;
    }
    return true;
}

int http_parse_headers(char *headers, struct http_request *request)
{
    char *line = next_line(&headers);
    if (line == NULL) {
        return 400;
    }
    int status = parse_request_line(line, request);
    if (status != 200) {
        return status;
    }

    bool has_length = false;
    bool has_transfer_encoding = false;
    bool has_expectation = false;

    while ((line = next_line(&headers)) != NULL && *line) {
        char *value = strchr(line, ':');
        if (value == NULL) {
            return 400;
        }
        *value++ = '\0';
        if (!is_token(line) || !is_header_value(value)) {
            return 400;
        }
        value = trim_whitespace(value);

        if (strcasecmp(line, "Host") == 0) {
            if (request->host != NULL || !is_host_value(value)) {
                return 400;
            }
            request->host = value;
        } else if (strcasecmp(line, "Content-Length") == 0) {
            if (has_length) {
                return 400;
            }
            has_length = true;
            status = parse_content_length(value, &request->content_length);
            if (status != 200) {
                return status;
            }
        } else if (strcasecmp(line, "Connection") == 0) {
            if (!parse_connection(value, request)) {
                return 400;
            }
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            has_transfer_encoding = true;
        } else if (strcasecmp(line, "Expect") == 0 && *value) {
            has_expectation = true;
        }
    }

    if (line == NULL || (has_length && has_transfer_encoding) ||
        (strcmp(request->version, "HTTP/1.1") == 0 && request->host == NULL)) {
        return 400;
    }
    if (has_transfer_encoding) {
        return 501;
    }
    if (has_expectation) {
        return 417;
    }
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "POST") != 0) {
        return 405;
    }
    if (*request->uri != '/' && strncmp(request->uri, "http://", 7) != 0 &&
        strncmp(request->uri, "https://", 8) != 0) {
        return 400;
    }
    return 200;
}

int http_write_response(char *response, size_t capacity, const char *version,
                        int status, const char *body)
{
    const char *reason;
    const char *allow = "";

    switch (status) {
    case 200: reason = "OK"; break;
    case 400: reason = "Bad Request"; break;
    case 405: reason = "Method Not Allowed"; allow = "Allow: GET, POST\r\n"; break;
    case 413: reason = "Content Too Large"; break;
    case 417: reason = "Expectation Failed"; break;
    case 431: reason = "Request Header Fields Too Large"; break;
    case 501: reason = "Not Implemented"; break;
    case 505: reason = "HTTP Version Not Supported"; break;
    default: status = 500; reason = "Internal Server Error"; break;
    }

    size_t body_length = strlen(body);
    const char *content_type = body_length > 0 ? "Content-Type: text/plain\r\n" : "";
    return snprintf(response, capacity,
                    "%s %d %s\r\n%sContent-Length: %zu\r\nConnection: close\r\n%s\r\n%s",
                    version == NULL ? "HTTP/1.1" : version, status, reason,
                    content_type, body_length, allow, body);
}
