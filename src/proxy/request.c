#include "proxy/request.h"
#include "proxy/headers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int proxy_request_build(const http_request *request, const char *backend_ipv4,
                          unsigned short port, proxy_request *output) {
  struct in_addr address;
  if (request == NULL || output == NULL || backend_ipv4 == NULL || port == 0 ||
      inet_pton(AF_INET, backend_ipv4, &address) != 1 ||
      request->header_count > HTTP_MAX_HEADERS ||
      request->body.length > HTTP_MAX_BODY_BYTES ||
      (request->body.data == NULL && request->body.length != 0) ||
      request->method.data == NULL || request->target.data == NULL ||
      request->target.length == 0 || request->target.length > HTTP_MAX_HEADER_BYTES ||
      request->target.data[0] != '/' ||
      memchr(request->target.data, '#', request->target.length) != NULL) {
    errno = EINVAL;
    return -1;
  }
  if (!(request->method.length == 3 && memcmp(request->method.data, "GET", 3) == 0) &&
      !(request->method.length == 4 && memcmp(request->method.data, "POST", 4) == 0)) {
    errno = ENOTSUP;
    return -1;
  }
  int removed[HTTP_MAX_HEADERS];
  if (proxy_headers_filter(request->headers, request->header_count, removed) == -1) {
    return -1;
  }
  for (size_t i = 0; i < request->header_count; ++i) {
    if (proxy_header_name_is(request->headers[i].name, "Host")) {
      removed[i] = 1;
    }
  }

  char *headers = malloc(HTTP_MAX_HEADER_BYTES + 1);
  if (headers == NULL) {
    return -1;
  }
  int length = snprintf(headers, HTTP_MAX_HEADER_BYTES + 1,
                         "%.*s %.*s HTTP/1.1\r\nHost: %s:%u\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n",
                         (int)request->method.length, request->method.data,
                         (int)request->target.length, request->target.data,
                         backend_ipv4, (unsigned int)port, request->body.length);
  if (length < 0 || length > HTTP_MAX_HEADER_BYTES) {
    free(headers);
    errno = length < 0 ? EIO : EMSGSIZE;
    return -1;
  }
  size_t used = (size_t)length;
  size_t header_count = 3;
  for (size_t i = 0; i < request->header_count; ++i) {
    if (removed[i]) {
      continue;
    }
    http_header header = request->headers[i];
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
  *output = (proxy_request){headers, used, request->body};
  return 0;
}
