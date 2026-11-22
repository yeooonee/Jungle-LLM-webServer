#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>

#define HTTP_MAX_HEADER_BYTES 8192
#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_BODY_BYTES (1024 * 1024)

typedef struct {
  const char *data;
  size_t length;
} http_slice;

typedef struct {
  http_slice name;
  http_slice value;
} http_header;

typedef struct {
  http_slice method;
  http_slice target;
  http_slice version;
  http_header headers[HTTP_MAX_HEADERS];
  size_t header_count;
  http_slice body;
  size_t consumed;
} http_request;

typedef enum {
  HTTP_PARSE_COMPLETE,
  HTTP_PARSE_INCOMPLETE,
  HTTP_PARSE_INVALID,
  HTTP_PARSE_TOO_LARGE,
  HTTP_PARSE_UNSUPPORTED
} http_parse_result;

/* 결과는 COMPLETE일 때만 유효하며, 원본 버퍼를 빌려 쓴다. 복사·해제하지 않는다. */
http_parse_result http_request_parse(const char *data, size_t length,
                                     http_request *request);

#endif
