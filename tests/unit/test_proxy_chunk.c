#include "proxy/body.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static proxy_body start(void) {
  const proxy_response response = {.status = 200, .body_kind = PROXY_BODY_CHUNKED};
  proxy_body body;
  assert(proxy_body_init(&body, &response) == 0);
  return body;
}

static void check_fragments(const char *wire, size_t length) {
  for (size_t split = 0; split <= length; ++split) {
    proxy_body body = start();
    http_slice forward;
    http_parse_result expected = split == length ? HTTP_PARSE_COMPLETE : HTTP_PARSE_INCOMPLETE;
    assert(proxy_body_consume(&body, (http_slice){wire, split}, 0, &forward) == expected);
    assert(forward.data == wire && forward.length == split);
    assert(proxy_body_consume(&body, (http_slice){wire + split, length - split}, 1,
                               &forward) == HTTP_PARSE_COMPLETE);
    assert(forward.data == wire + split && forward.length == length - split);
    assert(body.complete && body.chunk_state == PROXY_CHUNK_DONE);
  }
  proxy_body body = start();
  for (size_t i = 0; i < length; ++i) {
    http_slice forward;
    http_parse_result expected = i + 1 == length ? HTTP_PARSE_COMPLETE : HTTP_PARSE_INCOMPLETE;
    assert(proxy_body_consume(&body, (http_slice){wire + i, 1}, i + 1 == length,
                               &forward) == expected);
    assert(forward.data == wire + i && forward.length == 1);
  }
  for (size_t i = 0; i < length; ++i) {
    body = start();
    http_slice forward = {"unchanged", 9};
    assert(proxy_body_consume(&body, (http_slice){wire, i}, 1, &forward) == HTTP_PARSE_INVALID);
    assert(!body.complete && body.chunk_state == PROXY_CHUNK_SIZE && body.line_length == 0);
    assert(forward.length == 9);
  }
}

static void reject(const char *wire, size_t length, http_parse_result expected) {
  proxy_body body = start();
  http_slice forward = {"unchanged", 9};
  assert(proxy_body_consume(&body, (http_slice){wire, length}, 0, &forward) == expected);
  assert(!body.complete && body.chunk_state == PROXY_CHUNK_SIZE && body.line_length == 0);
  assert(forward.length == 9 && memcmp(forward.data, "unchanged", 9) == 0);
}

int main(void) {
  const char wire[] = "A;foo=bar;note=\"a; b\\\"c\"\r\n0123456789\r\n"
                      "6\r\nab\r\ncd\r\n3\r\nx\0y\r\n"
                      "0;end=yes\r\nX-Checksum: abc\r\n\r\n";
  check_fragments(wire, sizeof(wire) - 1);
  const char hex16[] = "10\r\n0123456789abcdef\r\n0\r\n\r\n";
  check_fragments(hex16, sizeof(hex16) - 1);
  const char hex26[] = "1a\r\nabcdefghijklmnopqrstuvwxyz\r\n000\r\n\r\n";
  check_fragments(hex26, sizeof(hex26) - 1);
  const char spaces[] = "1 ; foo = \"\" ; flag\r\nx\r\n0\r\n\r\n";
  check_fragments(spaces, sizeof(spaces) - 1);

  proxy_body body = start();
  http_slice forward;
  const char extra[] = "0\r\n\r\nnot part of body";
  assert(proxy_body_consume(&body, (http_slice){extra, sizeof(extra) - 1}, 0,
                             &forward) == HTTP_PARSE_COMPLETE);
  assert(forward.length == 5);
  assert(proxy_body_consume(&body, (http_slice){extra, sizeof(extra) - 1}, 1,
                             &forward) == HTTP_PARSE_COMPLETE && forward.length == 0);

  const char *invalid[] = {
      "G\r\n", "+1\r\n", "-1\r\n", "0x1\r\n", " 1\r\n", "\r\n", "1\n",
      "1\rX", "1\r\naX", "1\r\na\rX", "1;=bad\r\n", "1;name=\r\n",
      "1;name=\"unfinished\r\n", "1;name=\"bad\"extra\r\n", "1;name==bad\r\n",
      "1 \r\n", "1;flag \r\n", "1;name=\"value\" \r\n",
      "0\r\nMissingColon\r\n", "0\r\nX : value\r\n", "0\r\n folded: value\r\n",
      "0\r\nContent-Length: 0\r\n", "0\r\ntransfer-encoding: chunked\r\n",
      "0\r\nConnection: keep-alive\r\n", "0\r\nContent-Type: text/plain\r\n",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    reject(invalid[i], strlen(invalid[i]), HTTP_PARSE_INVALID);
  }
  const char nul[] = "0\r\nX: a\0b\r\n\r\n";
  reject(nul, sizeof(nul) - 1, HTTP_PARSE_INVALID);
  char maximum[128];
  int length = snprintf(maximum, sizeof(maximum), "%zx\r\n", SIZE_MAX);
  body = start();
  assert(proxy_body_consume(&body, (http_slice){maximum, (size_t)length}, 0,
                             &forward) == HTTP_PARSE_INCOMPLETE);
  assert(body.remaining == SIZE_MAX && body.chunk_state == PROXY_CHUNK_DATA);
  assert(proxy_body_consume(&body, (http_slice){"hello", 5}, 0,
                             &forward) == HTTP_PARSE_INCOMPLETE);
  assert(body.remaining == SIZE_MAX - 5);
  forward = (http_slice){"unchanged", 9};
  assert(proxy_body_consume(&body, (http_slice){NULL, 0}, 1, &forward) == HTTP_PARSE_INVALID);
  assert(body.remaining == SIZE_MAX - 5 && forward.length == 9);
  length = snprintf(maximum, sizeof(maximum), "%zx0\r\n", SIZE_MAX);
  reject(maximum, (size_t)length, HTTP_PARSE_INVALID);

  char *large = malloc(HTTP_MAX_HEADER_BYTES + 32);
  assert(large != NULL);
  memcpy(large, "0;x=", 4);
  memset(large + 4, 'a', HTTP_MAX_HEADER_BYTES - 6);
  memcpy(large + HTTP_MAX_HEADER_BYTES - 2, "\r\n\r\n", 4);
  body = start();
  assert(proxy_body_consume(&body, (http_slice){large, HTTP_MAX_HEADER_BYTES + 2}, 0,
                             &forward) == HTTP_PARSE_COMPLETE);
  large[HTTP_MAX_HEADER_BYTES - 2] = 'a';
  memcpy(large + HTTP_MAX_HEADER_BYTES - 1, "\r\n\r\n", 4);
  reject(large, HTTP_MAX_HEADER_BYTES + 3, HTTP_PARSE_TOO_LARGE);

  memcpy(large, "0\r\nX: ", 6);
  memset(large + 6, 'a', HTTP_MAX_HEADER_BYTES - 7);
  memcpy(large + HTTP_MAX_HEADER_BYTES - 1, "\r\n\r\n", 4);
  body = start();
  assert(proxy_body_consume(&body, (http_slice){large, HTTP_MAX_HEADER_BYTES + 3}, 0,
                             &forward) == HTTP_PARSE_COMPLETE);
  large[HTTP_MAX_HEADER_BYTES - 1] = 'a';
  memcpy(large + HTTP_MAX_HEADER_BYTES, "\r\n\r\n", 4);
  reject(large, HTTP_MAX_HEADER_BYTES + 4, HTTP_PARSE_TOO_LARGE);

  size_t used = 3;
  memcpy(large, "0\r\n", used);
  for (size_t i = 0; i < HTTP_MAX_HEADERS; ++i) {
    memcpy(large + used, "X: a\r\n", 6);
    used += 6;
  }
  memcpy(large + used, "\r\n", 2);
  body = start();
  assert(proxy_body_consume(&body, (http_slice){large, used + 2}, 0,
                             &forward) == HTTP_PARSE_COMPLETE);
  assert(body.trailer_count == HTTP_MAX_HEADERS);
  memcpy(large + used, "X: a\r\n\r\n", 8);
  reject(large, used + 8, HTTP_PARSE_TOO_LARGE);
  free(large);

  body = start();
  for (size_t i = 0; i < 4096; ++i) {
    assert(proxy_body_consume(&body, (http_slice){"1\r\nx\r\n", 6}, 0,
                               &forward) == HTTP_PARSE_INCOMPLETE);
    assert(forward.length == 6 && body.line_length == 0 && body.chunk_state == PROXY_CHUNK_SIZE);
  }
  assert(proxy_body_consume(&body, (http_slice){"0\r\n\r\n", 5}, 1,
                             &forward) == HTTP_PARSE_COMPLETE);
  puts("Proxy chunked body checks passed");
  return 0;
}
