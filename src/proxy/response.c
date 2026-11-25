#include "proxy/response.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

static int name_is(http_slice name, const char *expected) {
  return name.length == strlen(expected) &&
         strncasecmp(name.data, expected, name.length) == 0;
}

static int is_token(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') ||
         (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static http_parse_result read_line(const char *data, size_t length,
                                    size_t *offset, http_slice *line) {
  size_t limit = length < HTTP_MAX_HEADER_BYTES ? length : HTTP_MAX_HEADER_BYTES;
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
  return length >= HTTP_MAX_HEADER_BYTES ? HTTP_PARSE_TOO_LARGE : HTTP_PARSE_INCOMPLETE;
}

static int parse_status(http_slice line, proxy_response *response) {
  if (line.length < 13 ||
      (memcmp(line.data, "HTTP/1.1", 8) != 0 && memcmp(line.data, "HTTP/1.0", 8) != 0) ||
      line.data[8] != ' ' || line.data[9] < '1' || line.data[9] > '5' ||
      line.data[10] < '0' || line.data[10] > '9' ||
      line.data[11] < '0' || line.data[11] > '9' || line.data[12] != ' ') {
    return 0;
  }
  for (size_t i = 13; i < line.length; ++i) {
    unsigned char c = (unsigned char)line.data[i];
    if ((c < 32 && c != '\t') || c == 127) {
      return 0;
    }
  }
  response->version = (http_slice){line.data, 8};
  response->status = (line.data[9] - '0') * 100 +
                     (line.data[10] - '0') * 10 + line.data[11] - '0';
  response->reason = (http_slice){line.data + 13, line.length - 13};
  return 1;
}

static int parse_header(http_slice line, http_header *header) {
  const char *colon = memchr(line.data, ':', line.length);
  if (colon == NULL || colon == line.data) {
    return 0;
  }
  header->name = (http_slice){line.data, (size_t)(colon - line.data)};
  header->value = (http_slice){colon + 1, line.length - header->name.length - 1};
  for (size_t i = 0; i < header->name.length; ++i) {
    if (!is_token((unsigned char)header->name.data[i])) {
      return 0;
    }
  }
  for (size_t i = 0; i < header->value.length; ++i) {
    unsigned char c = (unsigned char)header->value.data[i];
    if ((c < 32 && c != '\t') || c == 127) {
      return 0;
    }
  }
  while (header->value.length > 0 &&
         (*header->value.data == ' ' || *header->value.data == '\t')) {
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

static int parse_length(http_slice value, size_t *length) {
  if (value.length == 0) {
    return 0;
  }
  size_t parsed = 0;
  for (size_t i = 0; i < value.length; ++i) {
    unsigned char c = (unsigned char)value.data[i];
    if (c < '0' || c > '9' || parsed > (SIZE_MAX - (c - '0')) / 10) {
      return 0;
    }
    parsed = parsed * 10 + (size_t)(c - '0');
  }
  *length = parsed;
  return 1;
}

http_parse_result proxy_response_parse_headers(const char *data, size_t length,
                                                proxy_response *response) {
  if (response == NULL || (data == NULL && length != 0)) {
    return HTTP_PARSE_INVALID;
  }
  if (length == 0) {
    return HTTP_PARSE_INCOMPLETE;
  }
  proxy_response parsed = {0};
  size_t offset = 0;
  http_slice line;
  http_parse_result result = read_line(data, length, &offset, &line);
  if (result != HTTP_PARSE_COMPLETE) {
    return result;
  }
  if (!parse_status(line, &parsed)) {
    return HTTP_PARSE_INVALID;
  }
  int has_length = 0;
  int has_encoding = 0;
  http_slice encoding = {0};
  for (;;) {
    result = read_line(data, length, &offset, &line);
    if (result != HTTP_PARSE_COMPLETE) {
      return result;
    }
    if (line.length == 0) {
      break;
    }
    if (parsed.header_count == HTTP_MAX_HEADERS) {
      return HTTP_PARSE_TOO_LARGE;
    }
    http_header header;
    if (!parse_header(line, &header)) {
      return HTTP_PARSE_INVALID;
    }
    parsed.headers[parsed.header_count++] = header;
    if (name_is(header.name, "Content-Length")) {
      if (has_length || !parse_length(header.value, &parsed.content_length)) {
        return HTTP_PARSE_INVALID;
      }
      has_length = 1;
    } else if (name_is(header.name, "Transfer-Encoding")) {
      if (has_encoding || header.value.length == 0) {
        return HTTP_PARSE_INVALID;
      }
      has_encoding = 1;
      encoding = header.value;
    }
  }
  if ((has_length && has_encoding) ||
      ((parsed.status < 200 || parsed.status == 204) && (has_length || has_encoding)) ||
      (has_encoding && memcmp(parsed.version.data, "HTTP/1.0", 8) == 0)) {
    return HTTP_PARSE_INVALID;
  }
  if (parsed.status == 101 || (has_encoding && !name_is(encoding, "chunked"))) {
    return HTTP_PARSE_UNSUPPORTED;
  }
  if (parsed.status < 200 || parsed.status == 204 || parsed.status == 304) {
    parsed.body_kind = PROXY_BODY_NONE;
  } else if (has_encoding) {
    parsed.body_kind = PROXY_BODY_CHUNKED;
  } else if (has_length) {
    parsed.body_kind = PROXY_BODY_LENGTH;
  } else {
    parsed.body_kind = PROXY_BODY_EOF;
  }
  parsed.consumed = offset;
  *response = parsed;
  return HTTP_PARSE_COMPLETE;
}
