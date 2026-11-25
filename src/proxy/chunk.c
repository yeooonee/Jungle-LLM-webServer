#include "proxy/chunk.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

static int hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int is_token(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') ||
         (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static void skip_space(http_slice line, size_t *offset) {
  while (*offset < line.length &&
         (line.data[*offset] == ' ' || line.data[*offset] == '\t')) {
    ++*offset;
  }
}

static int parse_size(http_slice line, size_t *size) {
  size_t offset = 0;
  size_t parsed = 0;
  while (offset < line.length) {
    int digit = hex_digit((unsigned char)line.data[offset]);
    if (digit < 0) {
      break;
    }
    if (parsed > (SIZE_MAX - (size_t)digit) / 16) {
      return 0;
    }
    parsed = parsed * 16 + (size_t)digit;
    ++offset;
  }
  if (offset == 0) {
    return 0;
  }
  while (offset < line.length) {
    skip_space(line, &offset);
    if (offset == line.length) {
      return 0;
    }
    if (line.data[offset++] != ';') {
      return 0;
    }
    skip_space(line, &offset);
    size_t start = offset;
    while (offset < line.length && is_token((unsigned char)line.data[offset])) {
      ++offset;
    }
    if (start == offset) {
      return 0;
    }
    size_t name_end = offset;
    skip_space(line, &offset);
    if (offset == line.length) {
      if (offset != name_end) {
        return 0;
      }
      break;
    }
    if (line.data[offset] != '=') {
      continue;
    }
    ++offset;
    skip_space(line, &offset);
    if (offset < line.length && line.data[offset] == '"') {
      int closed = 0;
      ++offset;
      while (offset < line.length) {
        unsigned char c = (unsigned char)line.data[offset++];
        if (c == '"') {
          closed = 1;
          break;
        }
        if (c == '\\') {
          if (offset == line.length) {
            return 0;
          }
          ++offset;
        }
      }
      if (!closed) {
        return 0;
      }
    } else {
      start = offset;
      while (offset < line.length && is_token((unsigned char)line.data[offset])) {
        ++offset;
      }
      if (start == offset) {
        return 0;
      }
    }
  }
  *size = parsed;
  return 1;
}

static int valid_trailer(http_slice line) {
  const char *colon = memchr(line.data, ':', line.length);
  if (colon == NULL || colon == line.data) {
    return 0;
  }
  size_t length = (size_t)(colon - line.data);
  for (size_t i = 0; i < length; ++i) {
    if (!is_token((unsigned char)line.data[i])) {
      return 0;
    }
  }
  const char *forbidden[] = {
      "Content-Length", "Transfer-Encoding", "Host", "Connection", "Trailer",
      "TE", "Upgrade", "Keep-Alive", "Proxy-Connection", "Authorization",
      "Proxy-Authorization", "Proxy-Authenticate", "WWW-Authenticate",
      "Content-Type", "Content-Encoding", "Content-Range", "Cache-Control",
  };
  for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
    if (strlen(forbidden[i]) == length && strncasecmp(line.data, forbidden[i], length) == 0) {
      return 0;
    }
  }
  return 1;
}

static http_parse_result finish_line(proxy_body *body) {
  http_slice line = {body->line, body->line_length};
  if (body->chunk_state == PROXY_CHUNK_SIZE) {
    if (!parse_size(line, &body->remaining)) {
      return HTTP_PARSE_INVALID;
    }
    body->chunk_state = body->remaining == 0 ? PROXY_CHUNK_TRAILERS : PROXY_CHUNK_DATA;
  } else if (line.length == 0) {
    body->chunk_state = PROXY_CHUNK_DONE;
    body->complete = 1;
  } else {
    if (++body->trailer_count > HTTP_MAX_HEADERS) {
      return HTTP_PARSE_TOO_LARGE;
    }
    if (!valid_trailer(line)) {
      return HTTP_PARSE_INVALID;
    }
  }
  body->line_length = 0;
  body->line_cr = 0;
  return HTTP_PARSE_COMPLETE;
}

http_parse_result proxy_chunk_consume(proxy_body *body, http_slice input, int eof,
                                        http_slice *forward) {
  proxy_body next = *body;
  size_t used = 0;
  while (used < input.length && !next.complete) {
    if (next.chunk_state == PROXY_CHUNK_DATA) {
      size_t amount = input.length - used;
      if (amount > next.remaining) {
        amount = next.remaining;
      }
      used += amount;
      next.remaining -= amount;
      if (next.remaining == 0) {
        next.chunk_state = PROXY_CHUNK_DATA_CR;
      }
      continue;
    }
    unsigned char c = (unsigned char)input.data[used++];
    if (next.chunk_state == PROXY_CHUNK_DATA_CR) {
      if (c != '\r') {
        return HTTP_PARSE_INVALID;
      }
      next.chunk_state = PROXY_CHUNK_DATA_LF;
    } else if (next.chunk_state == PROXY_CHUNK_DATA_LF) {
      if (c != '\n') {
        return HTTP_PARSE_INVALID;
      }
      next.chunk_state = PROXY_CHUNK_SIZE;
    } else if (next.chunk_state == PROXY_CHUNK_SIZE || next.chunk_state == PROXY_CHUNK_TRAILERS) {
      if (next.chunk_state == PROXY_CHUNK_TRAILERS &&
          ++next.trailer_bytes > HTTP_MAX_HEADER_BYTES) {
        return HTTP_PARSE_TOO_LARGE;
      }
      if (next.line_cr) {
        if (c != '\n') {
          return HTTP_PARSE_INVALID;
        }
        http_parse_result result = finish_line(&next);
        if (result != HTTP_PARSE_COMPLETE) {
          return result;
        }
      } else if (c == '\r') {
        next.line_cr = 1;
      } else if ((c < 32 && c != '\t') || c == 127) {
        return HTTP_PARSE_INVALID;
      } else {
        if (next.line_length == HTTP_MAX_HEADER_BYTES - 2) {
          return HTTP_PARSE_TOO_LARGE;
        }
        next.line[next.line_length++] = (char)c;
      }
    } else {
      return HTTP_PARSE_INVALID;
    }
  }
  if (eof && !next.complete) {
    return HTTP_PARSE_INVALID;
  }
  *body = next;
  *forward = (http_slice){input.data, used};
  return next.complete ? HTTP_PARSE_COMPLETE : HTTP_PARSE_INCOMPLETE;
}
