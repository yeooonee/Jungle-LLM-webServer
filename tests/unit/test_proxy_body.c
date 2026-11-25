#include "proxy/body.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static proxy_body start(const char *data, proxy_response *response) {
  assert(proxy_response_parse_headers(data, strlen(data), response) == HTTP_PARSE_COMPLETE);
  proxy_body body;
  assert(proxy_body_init(&body, response) == 0);
  return body;
}

int main(void) {
  proxy_response response;
  const char partial[] = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
  proxy_body body = start(partial, &response);
  assert(body.remaining == 10 && !body.complete);
  http_slice forward;
  http_slice first = {partial + response.consumed, sizeof(partial) - 1 - response.consumed};
  assert(proxy_body_consume(&body, first, 0, &forward) == HTTP_PARSE_INCOMPLETE);
  assert(body.remaining == 5 && forward.data == first.data && forward.length == 5);
  assert(memcmp(forward.data, "hello", 5) == 0);
  const char final[] = "worldextra";
  assert(proxy_body_consume(&body, (http_slice){final, sizeof(final) - 1}, 0,
                             &forward) == HTTP_PARSE_COMPLETE);
  assert(body.remaining == 0 && body.complete);
  assert(forward.data == final && forward.length == 5);
  assert(memcmp(forward.data, "world", 5) == 0);
  assert(proxy_body_consume(&body, (http_slice){final, sizeof(final) - 1}, 1,
                             &forward) == HTTP_PARSE_COMPLETE && forward.length == 0);

  body = start("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", &response);
  assert(proxy_body_consume(&body, (http_slice){NULL, 0}, 0, &forward) == HTTP_PARSE_INCOMPLETE);
  assert(body.remaining == 5 && forward.length == 0);
  const char binary[] = {'a', '\0', '\xff', '\r', '\n'};
  for (size_t i = 0; i < sizeof(binary); ++i) {
    http_parse_result expected = i == sizeof(binary) - 1 ? HTTP_PARSE_COMPLETE : HTTP_PARSE_INCOMPLETE;
    assert(proxy_body_consume(&body, (http_slice){binary + i, 1}, 0, &forward) == expected);
    assert(forward.length == 1 && forward.data == binary + i);
  }
  assert(body.complete);

  body = start("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", &response);
  assert(proxy_body_consume(&body, (http_slice){"hello", 5}, 0, &forward) == HTTP_PARSE_INCOMPLETE);
  assert(body.remaining == 5);
  forward = (http_slice){"unchanged", 9};
  assert(proxy_body_consume(&body, (http_slice){NULL, 0}, 1, &forward) == HTTP_PARSE_INVALID);
  assert(body.remaining == 5 && !body.complete && forward.length == 9);
  assert(proxy_body_consume(&body, (http_slice){"end", 3}, 1, &forward) == HTTP_PARSE_INVALID);
  assert(body.remaining == 5 && forward.length == 9);

  body = start("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", &response);
  assert(body.complete);
  assert(proxy_body_consume(&body, (http_slice){binary, sizeof(binary)}, 0,
                             &forward) == HTTP_PARSE_COMPLETE && forward.length == 0);
  body = start("HTTP/1.1 204 No Content\r\n\r\n", &response);
  assert(body.complete);
  assert(proxy_body_consume(&body, (http_slice){binary, sizeof(binary)}, 0,
                             &forward) == HTTP_PARSE_COMPLETE && forward.length == 0);
  body = start("HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\n\r\n", &response);
  assert(body.complete && body.remaining == 0);

  body = start("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n", &response);
  assert(!body.complete);
  assert(proxy_body_consume(&body, (http_slice){binary, sizeof(binary)}, 0,
                             &forward) == HTTP_PARSE_INCOMPLETE);
  assert(forward.data == binary && forward.length == sizeof(binary) && !body.complete);
  assert(proxy_body_consume(&body, (http_slice){NULL, 0}, 0, &forward) == HTTP_PARSE_INCOMPLETE);
  assert(proxy_body_consume(&body, (http_slice){NULL, 0}, 1, &forward) == HTTP_PARSE_COMPLETE);
  assert(body.complete && forward.length == 0);
  body = start("HTTP/1.1 200 OK\r\n\r\n", &response);
  assert(proxy_body_consume(&body, (http_slice){binary, sizeof(binary)}, 1,
                             &forward) == HTTP_PARSE_COMPLETE);
  assert(forward.length == sizeof(binary));

  response = (proxy_response){.body_kind = PROXY_BODY_LENGTH, .content_length = SIZE_MAX};
  assert(proxy_body_init(&body, &response) == 0);
  for (size_t i = 0; i < 1024; ++i) {
    assert(proxy_body_consume(&body, (http_slice){binary, sizeof(binary)}, 0,
                               &forward) == HTTP_PARSE_INCOMPLETE);
  }
  assert(body.remaining == SIZE_MAX - 1024 * sizeof(binary));
  assert(proxy_body_init(NULL, &response) == -1 && errno == EINVAL);
  assert(proxy_body_init(&body, NULL) == -1 && errno == EINVAL);
  response.body_kind = PROXY_BODY_CHUNKED;
  assert(proxy_body_init(&body, &response) == 0);
  assert(!body.complete && body.chunk_state == PROXY_CHUNK_SIZE);
  response.body_kind = (proxy_body_kind)99;
  size_t remaining = body.remaining;
  assert(proxy_body_init(&body, &response) == -1 && errno == EINVAL);
  assert(body.remaining == remaining);
  assert(proxy_body_consume(NULL, (http_slice){NULL, 0}, 0, &forward) == HTTP_PARSE_INVALID);
  assert(proxy_body_consume(&body, (http_slice){NULL, 1}, 0, &forward) == HTTP_PARSE_INVALID);
  assert(proxy_body_consume(&body, (http_slice){NULL, 0}, 0, NULL) == HTTP_PARSE_INVALID);
  puts("Proxy response body checks passed");
  return 0;
}
