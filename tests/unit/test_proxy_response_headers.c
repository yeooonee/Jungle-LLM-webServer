#include "proxy/response_headers.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static proxy_response_headers build(const char *data, proxy_body_kind expected) {
  proxy_response response;
  assert(proxy_response_parse_headers(data, strlen(data), &response) == HTTP_PARSE_COMPLETE);
  proxy_response_headers output;
  assert(proxy_response_build_headers(&response, &output) == 0);
  assert(output.data[output.length] == '\0');
  proxy_response reparsed;
  assert(proxy_response_parse_headers(output.data, output.length, &reparsed) == HTTP_PARSE_COMPLETE);
  assert(reparsed.status == response.status && reparsed.body_kind == expected);
  assert(reparsed.consumed == output.length);
  return output;
}

static void reject(const char *data, int error) {
  proxy_response response;
  assert(proxy_response_parse_headers(data, strlen(data), &response) == HTTP_PARSE_COMPLETE);
  proxy_response_headers output = {.length = 123};
  assert(proxy_response_build_headers(&response, &output) == -1);
  assert(errno == error && output.data == NULL && output.length == 123);
}

int main(void) {
  const char input[] = "HTTP/1.0 299 Custom result\r\nContent-Length: 0005\r\n"
                       "X-Temporary: removed\r\nConnection: keep-alive, X-Temporary\r\n"
                       "Keep-Alive: timeout=5\r\nContent-Type: text/plain\r\n"
                       "Set-Cookie: one=1\r\nSet-Cookie: two=2\r\nX-Note: a:b:c\r\n\r\nhello";
  proxy_response_headers output = build(input, PROXY_BODY_LENGTH);
  const char expected[] = "HTTP/1.1 299 Custom result\r\nConnection: close\r\n"
                          "Content-Length: 5\r\nContent-Type: text/plain\r\n"
                          "Set-Cookie: one=1\r\nSet-Cookie: two=2\r\nX-Note: a:b:c\r\n\r\n";
  assert(output.length == sizeof(expected) - 1);
  assert(memcmp(output.data, expected, output.length) == 0);
  free(output.data);

  const char chunked[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: ChUnKeD\r\n"
                         "Connection: X-One\r\nconnection: X-Two\r\nX-One: a\r\nX-Two: b\r\n"
                         "Trailer: X-Checksum\r\nTE: trailers\r\nUpgrade: ignored\r\n"
                         "Proxy-Connection: keep-alive\r\nProxy-Authenticate: ignored\r\n"
                         "Proxy-Authentication-Info: ignored\r\nContent-Type: text/plain\r\n\r\n";
  output = build(chunked, PROXY_BODY_CHUNKED);
  const char chunk_expected[] = "HTTP/1.1 200 OK\r\nConnection: close\r\n"
                                "Transfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n";
  assert(output.length == sizeof(chunk_expected) - 1);
  assert(memcmp(output.data, chunk_expected, output.length) == 0);
  free(output.data);

  output = build("HTTP/1.1 599 \r\n\r\n", PROXY_BODY_EOF);
  assert(strcmp(output.data, "HTTP/1.1 599 \r\nConnection: close\r\n\r\n") == 0);
  free(output.data);
  output = build("HTTP/1.1 204 No Content\r\n\r\n", PROXY_BODY_NONE);
  assert(strstr(output.data, "Content-Length:") == NULL);
  assert(strstr(output.data, "Transfer-Encoding:") == NULL);
  free(output.data);
  output = build("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", PROXY_BODY_LENGTH);
  assert(strstr(output.data, "Content-Length: 0\r\n") != NULL);
  free(output.data);
  output = build("HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n", PROXY_BODY_NONE);
  assert(strstr(output.data, "Content-Length: 100\r\n") != NULL);
  free(output.data);
  output = build("HTTP/1.1 304 Not Modified\r\n\r\n", PROXY_BODY_NONE);
  assert(strstr(output.data, "Content-Length:") == NULL);
  free(output.data);
  output = build("HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n", PROXY_BODY_NONE);
  assert(strstr(output.data, "Transfer-Encoding:") == NULL);
  free(output.data);

  reject("HTTP/1.1 100 Continue\r\n\r\n", ENOTSUP);
  reject("HTTP/1.1 200 OK\r\nConnection: X-One X-Two\r\n\r\n", EINVAL);
  proxy_response response;
  assert(proxy_response_parse_headers(input, sizeof(input) - 1, &response) == HTTP_PARSE_COMPLETE);
  assert(proxy_response_build_headers(NULL, &output) == -1 && errno == EINVAL);
  assert(proxy_response_build_headers(&response, NULL) == -1 && errno == EINVAL);

  output = build("HTTP/1.1 200 OK\r\nX: \r\n\r\n", PROXY_BODY_EOF);
  size_t padding = HTTP_MAX_HEADER_BYTES - output.length;
  free(output.data);
  char *large = malloc(HTTP_MAX_HEADER_BYTES + 32);
  assert(large != NULL);
  const char prefix[] = "HTTP/1.1 200 OK\r\nX: ";
  size_t used = sizeof(prefix) - 1;
  memcpy(large, prefix, used);
  memset(large + used, 'a', padding);
  memcpy(large + used + padding, "\r\n\r\n", 5);
  output = build(large, PROXY_BODY_EOF);
  assert(output.length == HTTP_MAX_HEADER_BYTES);
  free(output.data);
  large[used + padding] = 'a';
  memcpy(large + used + padding + 1, "\r\n\r\n", 5);
  reject(large, EMSGSIZE);

  used = strlen("HTTP/1.1 200 OK\r\n");
  memcpy(large, "HTTP/1.1 200 OK\r\n", used);
  for (size_t i = 0; i < HTTP_MAX_HEADERS - 1; ++i) {
    memcpy(large + used, "X: a\r\n", 6);
    used += 6;
  }
  memcpy(large + used, "\r\n", 3);
  output = build(large, PROXY_BODY_EOF);
  free(output.data);
  memcpy(large + used, "X: a\r\n\r\n", 9);
  reject(large, EMSGSIZE);
  free(large);
  puts("Proxy client response header checks passed");
  return 0;
}
