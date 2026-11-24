#include "cgi/response.h"

#include <string.h>
#include <strings.h>

static int name_is(http_slice name, const char *expected) {
  return name.length == strlen(expected) &&
         strncasecmp(name.data, expected, name.length) == 0;
}

static int valid_name(http_slice name) {
  if (name.length == 0) {
    return 0;
  }
  for (size_t i = 0; i < name.length; ++i) {
    unsigned char c = (unsigned char)name.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL))) {
      return 0;
    }
  }
  return 1;
}

int cgi_response_parse(const char *data, size_t length, cgi_response *response) {
  if (data == NULL || response == NULL ||
      length > HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_BYTES) {
    return 0;
  }
  cgi_response parsed = {.status = 200};
  int has_status = 0;
  size_t offset = 0;
  size_t header_count = 0;
  while (offset < length) {
    size_t end = offset;
    while (end < length && end < HTTP_MAX_HEADER_BYTES &&
           data[end] != '\r' && data[end] != '\n') {
      ++end;
    }
    if (end + 1 >= length || end + 2 > HTTP_MAX_HEADER_BYTES ||
        data[end] != '\r' || data[end + 1] != '\n') {
      return 0;
    }
    size_t line_length = end - offset;
    const char *line = data + offset;
    offset = end + 2;
    if (line_length == 0) {
      if (parsed.content_type.length == 0 ||
          length - offset > HTTP_MAX_BODY_BYTES) {
        return 0;
      }
      parsed.body = (http_slice){data + offset, length - offset};
      *response = parsed;
      return 1;
    }
    if (++header_count > HTTP_MAX_HEADERS) {
      return 0;
    }
    const char *colon = memchr(line, ':', line_length);
    if (colon == NULL) {
      return 0;
    }
    http_slice name = {line, (size_t)(colon - line)};
    if (!valid_name(name)) {
      return 0;
    }
    http_slice value = {colon + 1, line_length - name.length - 1};
    for (size_t i = 0; i < value.length; ++i) {
      unsigned char c = (unsigned char)value.data[i];
      if ((c < 32 && c != '\t') || c > 126) {
        return 0;
      }
    }
    while (value.length > 0 && (*value.data == ' ' || *value.data == '\t')) {
      ++value.data;
      --value.length;
    }
    while (value.length > 0 &&
           (value.data[value.length - 1] == ' ' || value.data[value.length - 1] == '\t')) {
      --value.length;
    }
    if (name_is(name, "Content-Type")) {
      if (parsed.content_type.length != 0 || value.length == 0 ||
          memchr(value.data, '\t', value.length) != NULL) {
        return 0;
      }
      parsed.content_type = value;
    } else if (name_is(name, "Status")) {
      if (has_status || value.length < 5 || value.data[0] < '2' ||
          value.data[0] > '5' || value.data[1] < '0' || value.data[1] > '9' ||
          value.data[2] < '0' || value.data[2] > '9' || value.data[3] != ' ' ||
          memchr(value.data, '\t', value.length) != NULL) {
        return 0;
      }
      parsed.status = (value.data[0] - '0') * 100 +
                      (value.data[1] - '0') * 10 + value.data[2] - '0';
      has_status = 1;
    }
  }
  return 0;
}
