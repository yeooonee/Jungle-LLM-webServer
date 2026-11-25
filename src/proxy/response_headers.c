#include "proxy/response_headers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "proxy/headers.h"

int proxy_response_build_headers(const proxy_response *response,
                                    proxy_response_headers *output) {
  if (response == NULL || output == NULL || response->header_count > HTTP_MAX_HEADERS ||
      response->reason.length > HTTP_MAX_HEADER_BYTES ||
      (response->reason.data == NULL && response->reason.length != 0) ||
      (response->body_kind != PROXY_BODY_NONE && response->body_kind != PROXY_BODY_LENGTH &&
       response->body_kind != PROXY_BODY_CHUNKED && response->body_kind != PROXY_BODY_EOF)) {
    errno = EINVAL;
    return -1;
  }
  if (response->status < 200 || response->status > 599) {
    errno = ENOTSUP;
    return -1;
  }
  int removed[HTTP_MAX_HEADERS];
  if (proxy_headers_filter(response->headers, response->header_count, removed) == -1) {
    return -1;
  }
  int has_length = response->body_kind == PROXY_BODY_LENGTH;
  if (response->status == 304) {
    for (size_t i = 0; i < response->header_count; ++i) {
      if (proxy_header_name_is(response->headers[i].name, "Content-Length")) {
        has_length = 1;
      }
    }
  }
  char *headers = malloc(HTTP_MAX_HEADER_BYTES + 1);
  if (headers == NULL) {
    return -1;
  }
  const char *reason = response->reason.data != NULL ? response->reason.data : "";
  int length = snprintf(headers, HTTP_MAX_HEADER_BYTES + 1,
                         "HTTP/1.1 %d %.*s\r\nConnection: close\r\n",
                         response->status, (int)response->reason.length, reason);
  if (length < 0 || length > HTTP_MAX_HEADER_BYTES) {
    free(headers);
    errno = length < 0 ? EIO : EMSGSIZE;
    return -1;
  }
  size_t used = (size_t)length;
  size_t header_count = 1;
  if (has_length) {
    char field[64];
    length = snprintf(field, sizeof(field), "Content-Length: %zu\r\n", response->content_length);
    if (length < 0 || (size_t)length >= sizeof(field) ||
        proxy_header_append(headers, &used, field, (size_t)length) == -1) {
      free(headers);
      errno = length < 0 ? EIO : EMSGSIZE;
      return -1;
    }
    ++header_count;
  } else if (response->body_kind == PROXY_BODY_CHUNKED) {
    const char field[] = "Transfer-Encoding: chunked\r\n";
    if (proxy_header_append(headers, &used, field, sizeof(field) - 1) == -1) {
      free(headers);
      errno = EMSGSIZE;
      return -1;
    }
    ++header_count;
  }
  for (size_t i = 0; i < response->header_count; ++i) {
    if (removed[i]) {
      continue;
    }
    http_header header = response->headers[i];
    if (++header_count > HTTP_MAX_HEADERS ||
        proxy_header_append(headers, &used, header.name.data, header.name.length) == -1 ||
        proxy_header_append(headers, &used, ": ", 2) == -1 ||
        proxy_header_append(headers, &used, header.value.data, header.value.length) == -1 ||
        proxy_header_append(headers, &used, "\r\n", 2) == -1) {
      free(headers);
      errno = EMSGSIZE;
      return -1;
    }
  }
  if (proxy_header_append(headers, &used, "\r\n", 2) == -1) {
    free(headers);
    errno = EMSGSIZE;
    return -1;
  }
  headers[used] = '\0';
  *output = (proxy_response_headers){headers, used};
  return 0;
}
