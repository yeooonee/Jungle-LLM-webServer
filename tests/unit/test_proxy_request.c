#include "proxy/request.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static proxy_request build(const char *data, size_t length) {
  http_request request;
  assert(http_request_parse(data, length, &request) == HTTP_PARSE_COMPLETE);
  proxy_request output;
  assert(proxy_request_build(&request, "127.0.0.1", 8001, &output) == 0);
  assert(output.body.data == request.body.data && output.body.length == request.body.length);
  assert(output.headers[output.headers_length] == '\0');
  return output;
}

static void reject(const char *data, int error) {
  http_request request;
  assert(http_request_parse(data, strlen(data), &request) == HTTP_PARSE_COMPLETE);
  proxy_request output = {.headers_length = 123};
  assert(proxy_request_build(&request, "127.0.0.1", 8001, &output) == -1);
  assert(errno == error && output.headers == NULL && output.headers_length == 123);
}

int main(void) {
  const char get[] = "GET /api/users?page=2 HTTP/1.1\r\nHost: localhost:8080\r\n"
                     "Connection: keep-alive\r\nAccept: application/json\r\n\r\n";
  proxy_request output = build(get, sizeof(get) - 1);
  const char expected[] = "GET /api/users?page=2 HTTP/1.1\r\nHost: 127.0.0.1:8001\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n"
                          "Accept: application/json\r\n\r\n";
  assert(output.headers_length == sizeof(expected) - 1);
  assert(memcmp(output.headers, expected, sizeof(expected) - 1) == 0);
  free(output.headers);

  const char post[] = "POST /api/add?x=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 0005\r\n"
                      "Content-Type: application/octet-stream\r\nX-Note: a:b:c\r\n\r\n"
                      "a\0\xff\r\n";
  output = build(post, sizeof(post) - 1);
  assert(strstr(output.headers, "Content-Length: 5\r\n") != NULL);
  assert(strstr(output.headers, "Content-Type: application/octet-stream\r\n") != NULL);
  assert(strstr(output.headers, "X-Note: a:b:c\r\n") != NULL);
  assert(output.body.length == 5 && memcmp(output.body.data, "a\0\xff\r\n", 5) == 0);
  char forwarded[512];
  memcpy(forwarded, output.headers, output.headers_length);
  memcpy(forwarded + output.headers_length, output.body.data, output.body.length);
  http_request reparsed;
  assert(http_request_parse(forwarded, output.headers_length + output.body.length,
                            &reparsed) == HTTP_PARSE_COMPLETE);
  assert(reparsed.body.length == 5 && memcmp(reparsed.body.data, output.body.data, 5) == 0);
  free(output.headers);

  const char hop[] = "GET / HTTP/1.1\r\nHost: x\r\nX-First: one\r\nX-Second: two\r\n"
                     "cOnNeCtIoN: , keep-alive,, X-FiRsT,\r\nConnection: X-Second\r\n"
                     "Keep-Alive: timeout=5\r\nProxy-Connection: keep-alive\r\n"
                     "TE: trailers\r\nTrailer: X-Trailer\r\nUpgrade: websocket\r\n"
                     "Proxy-Authorization: Basic hidden\r\nAuthorization: Bearer kept\r\n"
                     "Cookie: session=kept\r\nX-Keep: one\r\nX-Keep: two\r\n\r\n";
  output = build(hop, sizeof(hop) - 1);
  const char hop_expected[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1:8001\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n"
                              "Authorization: Bearer kept\r\nCookie: session=kept\r\n"
                              "X-Keep: one\r\nX-Keep: two\r\n\r\n";
  assert(output.headers_length == sizeof(hop_expected) - 1);
  assert(memcmp(output.headers, hop_expected, sizeof(hop_expected) - 1) == 0);
  free(output.headers);

  const char http10[] = "GET / HTTP/1.0\r\n\r\n";
  output = build(http10, sizeof(http10) - 1);
  assert(strncmp(output.headers, "GET / HTTP/1.1\r\n", 16) == 0);
  free(output.headers);
  reject("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", ENOTSUP);
  reject("get / HTTP/1.1\r\nHost: x\r\n\r\n", ENOTSUP);
  reject("GET http://example.com/ HTTP/1.1\r\nHost: x\r\n\r\n", EINVAL);
  reject("GET /path#fragment HTTP/1.1\r\nHost: x\r\n\r\n", EINVAL);
  reject("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive X-First\r\n\r\n", EINVAL);
  reject("GET / HTTP/1.1\r\nHost: x\r\nConnection: X:bad\r\n\r\n", EINVAL);
  reject("GET / HTTP/1.1\r\nHost: x\r\nConnection: \"X-First\"\r\n\r\n", EINVAL);

  http_request request;
  assert(http_request_parse(get, sizeof(get) - 1, &request) == HTTP_PARSE_COMPLETE);
  assert(proxy_request_build(NULL, "127.0.0.1", 8001, &output) == -1 && errno == EINVAL);
  assert(proxy_request_build(&request, NULL, 8001, &output) == -1 && errno == EINVAL);
  assert(proxy_request_build(&request, "localhost", 8001, &output) == -1 && errno == EINVAL);
  assert(proxy_request_build(&request, "127.0.0.1\r\nX: bad", 8001, &output) == -1);
  assert(errno == EINVAL);
  assert(proxy_request_build(&request, "127.0.0.1", 0, &output) == -1 && errno == EINVAL);
  assert(proxy_request_build(&request, "127.0.0.1", 8001, NULL) == -1 && errno == EINVAL);

  const char baseline[] = "GET / HTTP/1.1\r\nHost: x\r\nX: \r\n\r\n";
  output = build(baseline, sizeof(baseline) - 1);
  size_t padding = HTTP_MAX_HEADER_BYTES - output.headers_length;
  free(output.headers);
  char *large = malloc(HTTP_MAX_BODY_BYTES + HTTP_MAX_HEADER_BYTES + 1);
  assert(large != NULL);
  const char prefix[] = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
  size_t used = sizeof(prefix) - 1;
  memcpy(large, prefix, used);
  memset(large + used, 'a', padding);
  memcpy(large + used + padding, "\r\n\r\n", 5);
  output = build(large, used + padding + 4);
  assert(output.headers_length == HTTP_MAX_HEADER_BYTES);
  free(output.headers);
  large[used + padding] = 'a';
  memcpy(large + used + padding + 1, "\r\n\r\n", 5);
  reject(large, EMSGSIZE);

  used = (size_t)snprintf(large, HTTP_MAX_HEADER_BYTES, "GET / HTTP/1.1\r\nHost: x\r\n");
  for (size_t i = 0; i < HTTP_MAX_HEADERS - 3; ++i) {
    memcpy(large + used, "X: a\r\n", 6);
    used += 6;
  }
  memcpy(large + used, "\r\n", 3);
  output = build(large, used + 2);
  free(output.headers);
  memcpy(large + used, "X: a\r\n\r\n", 9);
  reject(large, EMSGSIZE);

  int header_length = snprintf(large, HTTP_MAX_HEADER_BYTES,
                               "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n",
                               HTTP_MAX_BODY_BYTES);
  memset(large + header_length, 0, HTTP_MAX_BODY_BYTES);
  output = build(large, (size_t)header_length + HTTP_MAX_BODY_BYTES);
  assert(output.body.length == HTTP_MAX_BODY_BYTES);
  assert(output.body.data == large + header_length);
  free(output.headers);
  free(large);
  puts("Proxy request construction checks passed");
  return 0;
}
