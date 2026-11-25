#include "proxy/body.h"
#include "proxy/chunk.h"

#include <errno.h>

int proxy_body_init(proxy_body *body, const proxy_response *response) {
  if (body == NULL || response == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (response->body_kind != PROXY_BODY_NONE &&
      response->body_kind != PROXY_BODY_LENGTH && response->body_kind != PROXY_BODY_EOF &&
      response->body_kind != PROXY_BODY_CHUNKED) {
    errno = EINVAL;
    return -1;
  }
  size_t remaining = response->body_kind == PROXY_BODY_LENGTH ? response->content_length : 0;
  int complete = response->body_kind == PROXY_BODY_NONE ||
                  (response->body_kind == PROXY_BODY_LENGTH && remaining == 0);
  *body = (proxy_body){.kind = response->body_kind, .remaining = remaining,
                       .complete = complete, .chunk_state = PROXY_CHUNK_SIZE};
  return 0;
}

http_parse_result proxy_body_consume(proxy_body *body, http_slice input, int eof,
                                       http_slice *forward) {
  if (body == NULL || forward == NULL || (input.data == NULL && input.length != 0)) {
    return HTTP_PARSE_INVALID;
  }
  if (body->kind == PROXY_BODY_CHUNKED) {
    return proxy_chunk_consume(body, input, eof, forward);
  }
  if (body->kind != PROXY_BODY_NONE && body->kind != PROXY_BODY_LENGTH &&
      body->kind != PROXY_BODY_EOF) {
    return HTTP_PARSE_INVALID;
  }
  proxy_body next = *body;
  size_t consumed = 0;
  if (!next.complete && next.kind == PROXY_BODY_LENGTH) {
    consumed = input.length < next.remaining ? input.length : next.remaining;
    next.remaining -= consumed;
    if (eof && next.remaining != 0) {
      return HTTP_PARSE_INVALID;
    }
    next.complete = next.remaining == 0;
  } else if (!next.complete && next.kind == PROXY_BODY_EOF) {
    consumed = input.length;
    next.complete = eof != 0;
  }
  *body = next;
  *forward = (http_slice){input.data, consumed};
  return next.complete ? HTTP_PARSE_COMPLETE : HTTP_PARSE_INCOMPLETE;
}
