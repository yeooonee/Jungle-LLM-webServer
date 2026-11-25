#include "proxy/response.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *data, int status, proxy_body_kind kind, size_t length) {
  proxy_response response;
  assert(proxy_response_parse_headers(data, strlen(data), &response) == HTTP_PARSE_COMPLETE);
  assert(response.status == status && response.body_kind == kind);
  assert(response.content_length == length);
  assert(response.consumed == (size_t)(strstr(data, "\r\n\r\n") - data) + 4);
}

static void reject(const char *data, http_parse_result expected) {
  proxy_response response = {.status = 777, .consumed = 123};
  assert(proxy_response_parse_headers(data, strlen(data), &response) == expected);
  assert(response.status == 777 && response.consumed == 123);
}

int main(void) {
  const char length_response[] = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n"
                                 "Content-Type: text/plain\r\n\r\nhello";
  size_t header_size = (size_t)(strstr(length_response, "\r\n\r\n") - length_response) + 4;
  for (size_t i = 0; i < header_size; ++i) {
    proxy_response response = {.status = 777};
    assert(proxy_response_parse_headers(length_response, i, &response) == HTTP_PARSE_INCOMPLETE);
    assert(response.status == 777);
  }
  check(length_response, 200, PROXY_BODY_LENGTH, 10);
  proxy_response response;
  assert(proxy_response_parse_headers(length_response, header_size, &response) == HTTP_PARSE_COMPLETE);
  assert(response.content_length == 10 && response.consumed == header_size);
  assert(response.reason.length == 2 && memcmp(response.reason.data, "OK", 2) == 0);
  assert(response.version.data == length_response && response.version.length == 8);

  check("HTTP/1.1 200 OK\r\nContent-Length:\t 000 \t\r\n\r\n", 200, PROXY_BODY_LENGTH, 0);
  check("HTTP/1.0 200 OK\r\n\r\nhello", 200, PROXY_BODY_EOF, 0);
  check("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello", 200, PROXY_BODY_EOF, 0);
  check("HTTP/1.1 201 Created\r\nContent-Length: 1048577\r\n\r\n", 201, PROXY_BODY_LENGTH, 1048577);
  check("HTTP/1.1 599 Custom\r\nContent-Length: 0\r\n\r\n", 599, PROXY_BODY_LENGTH, 0);
  check("HTTP/1.1 200 \r\n\r\n", 200, PROXY_BODY_EOF, 0);
  check("HTTP/1.1 200 OK\r\ntransfer-encoding:\t ChUnKeD \t\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        200, PROXY_BODY_CHUNKED, 0);
  check("HTTP/1.1 204 No Content\r\n\r\nignored", 204, PROXY_BODY_NONE, 0);
  check("HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n", 304, PROXY_BODY_NONE, 100);
  check("HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n", 304, PROXY_BODY_NONE, 0);
  check("HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n", 103, PROXY_BODY_NONE, 0);

  const char combined[] = "HTTP/1.1 100 Continue\r\n\r\n"
                          "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  assert(proxy_response_parse_headers(combined, sizeof(combined) - 1, &response) == HTTP_PARSE_COMPLETE);
  assert(response.status == 100 && response.body_kind == PROXY_BODY_NONE);
  size_t interim = response.consumed;
  assert(interim == strlen("HTTP/1.1 100 Continue\r\n\r\n"));
  assert(proxy_response_parse_headers(combined + interim, sizeof(combined) - 1 - interim,
                                      &response) == HTTP_PARSE_COMPLETE);
  assert(response.status == 200 && response.content_length == 5);
  assert(memcmp(combined + interim + response.consumed, "hello", 5) == 0);

  const char binary[] = "HTTP/1.1 200 OK\r\nX-Note: a:b:c\r\nX-Bytes: \xff\r\n"
                        "Content-Length: 5\r\n\r\na\0\xff\r\n";
  assert(proxy_response_parse_headers(binary, sizeof(binary) - 1, &response) == HTTP_PARSE_COMPLETE);
  assert(response.header_count == 3 && response.headers[0].value.length == 5);
  assert(memcmp(response.headers[0].value.data, "a:b:c", 5) == 0);
  assert(response.headers[1].value.length == 1 && (unsigned char)*response.headers[1].value.data == 255);
  assert(response.consumed + 5 == sizeof(binary) - 1);
  assert(memcmp(binary + response.consumed, "a\0\xff\r\n", 5) == 0);

  const char *invalid[] = {
      "HTTP/2.0 200 OK\r\n\r\n", "HTTP/1.1 099 Bad\r\n\r\n",
      "HTTP/1.1 600 Bad\r\n\r\n", "HTTP/1.1 20x Bad\r\n\r\n",
      "HTTP/1.1 2000 Bad\r\n\r\n", "HTTP/1.1 200\r\n\r\n",
      "HTTP/1.1 200 bad\x01\r\n\r\n", "HTTP/1.1 200 OK\n\n",
      "HTTP/1.1 200 OK\rX", "HTTP/1.1 200 OK\r\nBad\r\n\r\n",
      "HTTP/1.1 200 OK\r\nX : bad\r\n\r\n", "HTTP/1.1 200 OK\r\n: bad\r\n\r\n",
      "HTTP/1.1 200 OK\r\n folded: bad\r\n\r\n", "HTTP/1.1 200 OK\r\nX: \177\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: +1\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 1 0\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\ncontent-length: 5\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: \r\n\r\n",
      "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
      "HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n",
      "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n",
      "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    reject(invalid[i], HTTP_PARSE_INVALID);
  }
  reject("HTTP/1.1 101 Switching Protocols\r\n\r\n", HTTP_PARSE_UNSUPPORTED);
  reject("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", HTTP_PARSE_UNSUPPORTED);
  reject("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", HTTP_PARSE_UNSUPPORTED);
  const char nul[] = "HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n";
  assert(proxy_response_parse_headers(nul, sizeof(nul) - 1, &response) == HTTP_PARSE_INVALID);
  char boundary[128];
  snprintf(boundary, sizeof(boundary), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", SIZE_MAX);
  check(boundary, 200, PROXY_BODY_LENGTH, SIZE_MAX);
  snprintf(boundary, sizeof(boundary), "HTTP/1.1 200 OK\r\nContent-Length: %zu0\r\n\r\n", SIZE_MAX);
  reject(boundary, HTTP_PARSE_INVALID);

  char *large = malloc(HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_BYTES + 1);
  assert(large != NULL);
  const char prefix[] = "HTTP/1.1 200 OK\r\nX: ";
  size_t start = sizeof(prefix) - 1;
  memcpy(large, prefix, start);
  memset(large + start, 'a', HTTP_MAX_HEADER_BYTES - start - 4);
  memcpy(large + HTTP_MAX_HEADER_BYTES - 4, "\r\n\r\n", 4);
  assert(proxy_response_parse_headers(large, HTTP_MAX_HEADER_BYTES - 1, &response) == HTTP_PARSE_INCOMPLETE);
  assert(proxy_response_parse_headers(large, HTTP_MAX_HEADER_BYTES, &response) == HTTP_PARSE_COMPLETE);
  assert(response.consumed == HTTP_MAX_HEADER_BYTES);
  memset(large + HTTP_MAX_HEADER_BYTES, 0, HTTP_MAX_BODY_BYTES + 1);
  assert(proxy_response_parse_headers(large, HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_BYTES + 1,
                                      &response) == HTTP_PARSE_COMPLETE);
  assert(response.body_kind == PROXY_BODY_EOF && response.consumed == HTTP_MAX_HEADER_BYTES);
  large[HTTP_MAX_HEADER_BYTES - 4] = 'a';
  memcpy(large + HTTP_MAX_HEADER_BYTES - 3, "\r\n\r\n", 4);
  response = (proxy_response){.status = 777};
  assert(proxy_response_parse_headers(large, HTTP_MAX_HEADER_BYTES + 1, &response) == HTTP_PARSE_TOO_LARGE);
  assert(response.status == 777);

  size_t used = strlen("HTTP/1.1 200 OK\r\n");
  memcpy(large, "HTTP/1.1 200 OK\r\n", used);
  for (size_t i = 0; i < HTTP_MAX_HEADERS; ++i) {
    memcpy(large + used, "X: a\r\n", 6);
    used += 6;
  }
  memcpy(large + used, "\r\n", 2);
  assert(proxy_response_parse_headers(large, used + 2, &response) == HTTP_PARSE_COMPLETE);
  assert(response.header_count == HTTP_MAX_HEADERS);
  memcpy(large + used, "X: a\r\n\r\n", 8);
  assert(proxy_response_parse_headers(large, used + 8, &response) == HTTP_PARSE_TOO_LARGE);
  free(large);
  assert(proxy_response_parse_headers(NULL, 0, &response) == HTTP_PARSE_INCOMPLETE);
  assert(proxy_response_parse_headers(NULL, 1, &response) == HTTP_PARSE_INVALID);
  assert(proxy_response_parse_headers(length_response, sizeof(length_response) - 1, NULL) == HTTP_PARSE_INVALID);
  puts("Proxy response header checks passed");
  return 0;
}
