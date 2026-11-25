#ifndef PROXY_RESPONSE_H
#define PROXY_RESPONSE_H

#include "http/request.h"

typedef enum {
  PROXY_BODY_NONE,
  PROXY_BODY_LENGTH,
  PROXY_BODY_CHUNKED,
  PROXY_BODY_EOF
} proxy_body_kind;

typedef struct {
  http_slice version;
  int status;
  http_slice reason;
  http_header headers[HTTP_MAX_HEADERS];
  size_t header_count;
  proxy_body_kind body_kind;
  size_t content_length;
  size_t consumed;
} proxy_response;

/* GET·POST에 대한 응답 헤더 한 블록만 해석한다. 본문·다음 응답은 소비하지 않는다.
 * COMPLETE일 때만 결과를 저장하며 slice는 원본 버퍼를 빌려 쓴다.
 * 1xx는 호출자가 consumed 뒤에서 다음 응답을 읽는다. 101은 UNSUPPORTED다. */
http_parse_result proxy_response_parse_headers(const char *data, size_t length,
                                                proxy_response *response);

#endif
