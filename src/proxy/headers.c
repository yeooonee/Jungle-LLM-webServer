#include "proxy/headers.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

static int same_name(http_slice left, http_slice right) {
  return left.length == right.length &&
         strncasecmp(left.data, right.data, left.length) == 0;
}

int proxy_header_name_is(http_slice name, const char *text) {
  return same_name(name, (http_slice){text, strlen(text)});
}

static int is_token(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') ||
         (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int remove_options(const http_header *headers, size_t count, http_slice value,
                            int removed[HTTP_MAX_HEADERS]) {
  size_t offset = 0;
  while (offset < value.length) {
    while (offset < value.length &&
           (value.data[offset] == ' ' || value.data[offset] == '\t' ||
            value.data[offset] == ',')) {
      ++offset;
    }
    if (offset == value.length) {
      break;
    }
    size_t start = offset;
    while (offset < value.length && is_token((unsigned char)value.data[offset])) {
      ++offset;
    }
    if (offset == start) {
      errno = EINVAL;
      return -1;
    }
    http_slice option = {value.data + start, offset - start};
    while (offset < value.length &&
           (value.data[offset] == ' ' || value.data[offset] == '\t')) {
      ++offset;
    }
    if (offset < value.length && value.data[offset] != ',') {
      errno = EINVAL;
      return -1;
    }
    for (size_t i = 0; i < count; ++i) {
      if (same_name(headers[i].name, option)) {
        removed[i] = 1;
      }
    }
  }
  return 0;
}

int proxy_header_append(char *headers, size_t *used, const char *data, size_t length) {
  if (*used > HTTP_MAX_HEADER_BYTES || length > HTTP_MAX_HEADER_BYTES - *used) {
    errno = EMSGSIZE;
    return -1;
  }
  if (length != 0) {
    memcpy(headers + *used, data, length);
  }
  *used += length;
  return 0;
}

int proxy_headers_filter(const http_header *headers, size_t count,
                            int removed[HTTP_MAX_HEADERS]) {
  if (count > HTTP_MAX_HEADERS || removed == NULL || (headers == NULL && count != 0)) {
    errno = EINVAL;
    return -1;
  }
  const char *names[] = {
      "Content-Length", "Transfer-Encoding", "Connection", "Proxy-Connection",
      "Keep-Alive", "TE", "Trailer", "Upgrade", "Proxy-Authorization",
      "Proxy-Authenticate", "Proxy-Authentication-Info",
  };
  memset(removed, 0, count * sizeof(*removed));
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); ++j) {
      if (proxy_header_name_is(headers[i].name, names[j])) {
        removed[i] = 1;
      }
    }
    if (proxy_header_name_is(headers[i].name, "Connection") &&
        remove_options(headers, count, headers[i].value, removed) == -1) {
      return -1;
    }
  }
  return 0;
}
