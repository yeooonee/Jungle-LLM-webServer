#include "http/request.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void equals(http_slice slice, const char *text) {
  assert(slice.length == strlen(text));
  assert(memcmp(slice.data, text, slice.length) == 0);
}

int main(void) {
  http_request request;
  const char *get = "GET /search?q=socket HTTP/1.1\r\nhost: example.com:8080\r\n"
                    "X-Note: a:b:c\r\nConnection: close\r\n\r\n";
  assert(http_request_parse(get, strlen(get), &request) == HTTP_PARSE_COMPLETE);
  equals(request.method, "GET");
  equals(request.target, "/search?q=socket");
  equals(request.version, "HTTP/1.1");
  assert(request.header_count == 3);
  equals(request.headers[0].name, "host");
  equals(request.headers[0].value, "example.com:8080");
  equals(request.headers[1].value, "a:b:c");
  equals(request.headers[2].value, "close");
  assert(request.body.length == 0 && request.consumed == strlen(get));

  const char post[] = "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                      "cOnTeNt-LeNgTh:\t 5 \t\r\n\r\nhello";
  for (size_t i = 0; i < sizeof(post) - 1; ++i) {
    assert(http_request_parse(post, i, &request) == HTTP_PARSE_INCOMPLETE);
  }
  assert(http_request_parse(post, sizeof(post) - 1, &request) == HTTP_PARSE_COMPLETE);
  equals(request.method, "POST");
  equals(request.body, "hello");
  equals(request.headers[1].value, "5");
  assert(request.body.data == post + sizeof(post) - 6);

  const char binary[] = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n"
                        "a\0\xff\r\nGET /next HTTP/1.1\r\nHost: x\r\n\r\n";
  assert(http_request_parse(binary, sizeof(binary) - 1, &request) == HTTP_PARSE_COMPLETE);
  assert(request.body.length == 5);
  assert(memcmp(request.body.data, "a\0\xff\r\n", 5) == 0);
  size_t consumed = request.consumed;
  assert(http_request_parse(binary + consumed, sizeof(binary) - 1 - consumed,
                            &request) == HTTP_PARSE_COMPLETE);
  equals(request.target, "/next");
  assert(request.body.length == 0);

  const char *cases[] = {
      "GET / HTTP/1.1\r\n\r\n", /* HTTP/1.1에는 Host가 필요하다. */
      "GET / HTTP/1.1\r\nHost: \t\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: x\r\nhost: y\r\n\r\n",
      "GET / HTTP/1.1\r\nHost : x\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: x\r\n folded: value\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: x\r\nMissingColon\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: x\r\n: empty-name\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: x\r\nBad(Name: value\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: x\r\nX: bad\x01\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: x\r\nX: bad\x7f\r\n\r\n",
      "GET / HTTP/1.1\nHost: x\n\n",
      "GET / HTTP/1.1\rX",
      "GET / HTTP/2.0\r\nHost: x\r\n\r\n",
      "GET / HTTP/1.1 extra\r\nHost: x\r\n\r\n",
      "GET  / HTTP/1.1\r\nHost: x\r\n\r\n",
      "GET /\t HTTP/1.1\r\nHost: x\r\n\r\n",
      "G(ET / HTTP/1.1\r\nHost: x\r\n\r\n",
      " / HTTP/1.1\r\nHost: x\r\n\r\n",
      "GET /\r\nHost: x\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5abc\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +5\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: \r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1 0\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 9999999999999999999999999\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 10\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\ncontent-length: 5\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5, 5\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (http_request_parse(cases[i], strlen(cases[i]), &request) != HTTP_PARSE_INVALID) {
      fprintf(stderr, "Invalid request case failed: %zu\n", i);
      abort();
    }
  }
  const char nul_header[] = "GET / HTTP/1.1\r\nHost: x\r\nX: a\0b\r\n\r\n";
  assert(http_request_parse(nul_header, sizeof(nul_header) - 1, &request) == HTTP_PARSE_INVALID);
  const char nul_method[] = "G\0T / HTTP/1.1\r\nHost: x\r\n\r\n";
  assert(http_request_parse(nul_method, sizeof(nul_method) - 1, &request) == HTTP_PARSE_INVALID);

  const char *http10 = "GET / HTTP/1.0\r\n\r\n";
  assert(http_request_parse(http10, strlen(http10), &request) == HTTP_PARSE_COMPLETE);
  const char *zero = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 000\r\n\r\nGET";
  assert(http_request_parse(zero, strlen(zero), &request) == HTTP_PARSE_COMPLETE);
  assert(request.body.length == 0 && request.consumed == strlen(zero) - 3);
  const char *chunked = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
  assert(http_request_parse(chunked, strlen(chunked), &request) == HTTP_PARSE_UNSUPPORTED);
  const char *oversized = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n";
  assert(http_request_parse(oversized, strlen(oversized), &request) == HTTP_PARSE_TOO_LARGE);

  char headers[HTTP_MAX_HEADER_BYTES + 1];
  const char *prefix = "GET / HTTP/1.1\r\nHost: x\r\nX: ";
  size_t start = strlen(prefix);
  memcpy(headers, prefix, start);
  memset(headers + start, 'a', HTTP_MAX_HEADER_BYTES - start - 4);
  memcpy(headers + HTTP_MAX_HEADER_BYTES - 4, "\r\n\r\n", 4);
  assert(http_request_parse(headers, HTTP_MAX_HEADER_BYTES, &request) == HTTP_PARSE_COMPLETE);
  assert(http_request_parse(headers, HTTP_MAX_HEADER_BYTES - 1, &request) == HTTP_PARSE_INCOMPLETE);
  headers[HTTP_MAX_HEADER_BYTES - 4] = 'a';
  memcpy(headers + HTTP_MAX_HEADER_BYTES - 3, "\r\n\r\n", 4);
  assert(http_request_parse(headers, sizeof(headers), &request) == HTTP_PARSE_TOO_LARGE);

  size_t offset = (size_t)snprintf(headers, sizeof(headers), "GET / HTTP/1.1\r\nHost: x\r\n");
  for (size_t i = 1; i < HTTP_MAX_HEADERS; ++i) {
    memcpy(headers + offset, "X: y\r\n", 6);
    offset += 6;
  }
  memcpy(headers + offset, "\r\n", 2);
  assert(http_request_parse(headers, offset + 2, &request) == HTTP_PARSE_COMPLETE);
  assert(request.header_count == HTTP_MAX_HEADERS);
  memcpy(headers + offset, "X: y\r\n\r\n", 8);
  assert(http_request_parse(headers, offset + 8, &request) == HTTP_PARSE_TOO_LARGE);

  char *large = malloc(HTTP_MAX_BODY_BYTES + 128);
  assert(large != NULL);
  int header_size = snprintf(large, HTTP_MAX_BODY_BYTES + 128,
                            "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n",
                            HTTP_MAX_BODY_BYTES);
  memset(large + header_size, 'z', HTTP_MAX_BODY_BYTES);
  assert(http_request_parse(large, (size_t)header_size + HTTP_MAX_BODY_BYTES,
                            &request) == HTTP_PARSE_COMPLETE);
  assert(request.body.length == HTTP_MAX_BODY_BYTES);
  free(large);

  assert(http_request_parse(NULL, 0, &request) == HTTP_PARSE_INCOMPLETE);
  assert(http_request_parse(NULL, 1, &request) == HTTP_PARSE_INVALID);
  assert(http_request_parse(get, strlen(get), NULL) == HTTP_PARSE_INVALID);
  puts("HTTP parser checks passed");
  return 0;
}
