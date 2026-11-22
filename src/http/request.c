#include "request.h"

#include <stdint.h>
#include <string.h>

static http_parse_result read_line(const char *data, size_t length,
                                   size_t *offset, http_slice *line);
static int parse_request_line(http_slice line, http_request *request);
static int parse_content_length(http_slice value, size_t *body_length);
static http_parse_result parse_headers(const char *data, size_t length,
                                       size_t *offset, http_request *request,
                                       size_t *body_length);

http_parse_result http_request_parse(const char *data, size_t length,
                                     http_request *request) {
  if (request == NULL || (data == NULL && length != 0)) {
    return HTTP_PARSE_INVALID;
  }
  if (length == 0) {
    return HTTP_PARSE_INCOMPLETE;
  }

  *request = (http_request){0};

  size_t offset = 0;
  http_slice line;
  http_parse_result status = read_line(data, length, &offset, &line);
  if (status != HTTP_PARSE_COMPLETE) {
    return status;
  }
  if (!parse_request_line(line, request)) {
    return HTTP_PARSE_INVALID;
  }

  size_t body_length = 0;
  status = parse_headers(data, length, &offset, request, &body_length);
  if (status != HTTP_PARSE_COMPLETE) {
    return status;
  }
  if (length - offset < body_length) {
    return HTTP_PARSE_INCOMPLETE;
  }
  request->body = (http_slice){data + offset, body_length};
  request->consumed = offset + body_length;
  return HTTP_PARSE_COMPLETE;
}

static int is_token(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int name_is(http_slice name, const char *expected) {
  size_t length = strlen(expected);
  if (name.length != length) {
    return 0;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)name.data[i];
    if (c >= 'A' && c <= 'Z') {
      c = (unsigned char)(c + ('a' - 'A'));
    }
    if (c != (unsigned char)expected[i]) {
      return 0;
    }
  }
  return 1;
}

/* 헤더만 한도 내에서 검색하고 본문은 줄바꿈과 무관한 바이트로 취급. */
static http_parse_result read_line(const char *data, size_t length,
                                   size_t *offset, http_slice *line) {
  size_t limit =
      length < HTTP_MAX_HEADER_BYTES ? length : HTTP_MAX_HEADER_BYTES;
  for (size_t i = *offset; i < limit; ++i) {
    if (data[i] == '\n') {
      return HTTP_PARSE_INVALID;
    }
    if (data[i] != '\r') {
      continue;
    }
    if (i + 1 >= limit) {
      break;
    }
    if (data[i + 1] != '\n') {
      return HTTP_PARSE_INVALID;
    }
    *line = (http_slice){data + *offset, i - *offset};
    *offset = i + 2;
    return HTTP_PARSE_COMPLETE;
  }
  return length >= HTTP_MAX_HEADER_BYTES ? HTTP_PARSE_TOO_LARGE
                                         : HTTP_PARSE_INCOMPLETE;
}

static int parse_request_line(http_slice line, http_request *request) {
  const char *method_end = memchr(line.data, ' ', line.length);
  if (method_end == NULL || method_end == line.data) {
    return 0;
  }

  size_t remaining = line.length - (size_t)(method_end + 1 - line.data);
  const char *target_end = memchr(method_end + 1, ' ', remaining);
  if (target_end == NULL || target_end == method_end + 1) {
    return 0;
  }

  request->method = (http_slice){line.data, (size_t)(method_end - line.data)};
  request->target =
      (http_slice){method_end + 1, (size_t)(target_end - method_end - 1)};
  request->version = (http_slice){
      target_end + 1, line.length - (size_t)(target_end + 1 - line.data)};

  for (size_t i = 0; i < request->method.length; ++i) {
    if (!is_token((unsigned char)request->method.data[i])) {
      return 0;
    }
  }
  for (size_t i = 0; i < request->target.length; ++i) {
    unsigned char c = (unsigned char)request->target.data[i];
    if (c <= 0x20 || c >= 0x7f) {
      return 0;
    }
  }

  return request->version.length == 8 &&
         (memcmp(request->version.data, "HTTP/1.1", 8) == 0 ||
          memcmp(request->version.data, "HTTP/1.0", 8) == 0);
}

static int parse_header(http_slice line, http_header *header) {
  const char *colon = memchr(line.data, ':', line.length);
  if (colon == NULL || colon == line.data) {
    return 0;
  }

  *header = (http_header){
      .name = {line.data, (size_t)(colon - line.data)},
      .value = {colon + 1, line.length - (size_t)(colon + 1 - line.data)},
  };

  for (size_t i = 0; i < header->name.length; ++i) {
    if (!is_token((unsigned char)header->name.data[i])) {
      return 0;
    }
  }

  for (size_t i = 0; i < header->value.length; ++i) {
    unsigned char c = (unsigned char)header->value.data[i];
    if ((c < 0x20 && c != '\t') || c == 0x7f) {
      return 0;
    }
  }

  while (header->value.length > 0 &&
         (header->value.data[0] == ' ' || header->value.data[0] == '\t')) {
    ++header->value.data;
    --header->value.length;
  }

  while (header->value.length > 0 &&
         (header->value.data[header->value.length - 1] == ' ' ||
          header->value.data[header->value.length - 1] == '\t')) {
    --header->value.length;
  }
  return 1;
}

static int parse_content_length(http_slice value, size_t *body_length) {
  if (value.length == 0) {
    return 0;
  }
  size_t parsed = 0;
  for (size_t i = 0; i < value.length; ++i) {
    unsigned char c = (unsigned char)value.data[i];
    if (c < '0' || c > '9' || parsed > (SIZE_MAX - (c - '0')) / 10) {
      return 0;
    }
    parsed = parsed * 10 + (c - '0');
  }
  *body_length = parsed;
  return 1;
}

static http_parse_result parse_headers(const char *data, size_t length,
                                       size_t *offset, http_request *request,
                                       size_t *body_length) {
  int has_host = 0;
  int has_length = 0;
  int has_transfer_encoding = 0;
  for (;;) {
    http_slice line;
    http_parse_result status = read_line(data, length, offset, &line);
    if (status != HTTP_PARSE_COMPLETE) {
      return status;
    }
    if (line.length == 0) {
      break;
    }
    if (request->header_count == HTTP_MAX_HEADERS) {
      return HTTP_PARSE_TOO_LARGE;
    }

    http_header header;
    if (!parse_header(line, &header)) {
      return HTTP_PARSE_INVALID;
    }

    request->headers[request->header_count++] = header;
    if (name_is(header.name, "host")) {
      if (has_host || header.value.length == 0) {
        return HTTP_PARSE_INVALID;
      }
      has_host = 1;
    } else if (name_is(header.name, "content-length")) {
      /* 프레이밍을 하나로 결정하도록 같은 값이어도 중복 길이 헤더는 거부. */
      if (has_length) {
        return HTTP_PARSE_INVALID;
      }

      has_length = 1;
      if (!parse_content_length(header.value, body_length)) {
        return HTTP_PARSE_INVALID;
      }
    } else if (name_is(header.name, "transfer-encoding")) {
      has_transfer_encoding = 1;
    }
  }
  if ((!has_host && memcmp(request->version.data, "HTTP/1.1", 8) == 0) ||
      (has_transfer_encoding && has_length)) {
    return HTTP_PARSE_INVALID;
  }
  if (has_transfer_encoding) {
    /* shortcut: Transfer-Encoding은 거부하며, chunked 지원 단계에서 해독을
     * 추가. */
    return HTTP_PARSE_UNSUPPORTED;
  }
  if (*body_length > HTTP_MAX_BODY_BYTES) {
    return HTTP_PARSE_TOO_LARGE;
  }
  return HTTP_PARSE_COMPLETE;
}
