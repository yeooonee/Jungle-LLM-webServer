#ifndef PROXY_BODY_H
#define PROXY_BODY_H

#include "proxy/response.h"

typedef enum {
  PROXY_CHUNK_SIZE,
  PROXY_CHUNK_DATA,
  PROXY_CHUNK_DATA_CR,
  PROXY_CHUNK_DATA_LF,
  PROXY_CHUNK_TRAILERS,
  PROXY_CHUNK_DONE
} proxy_chunk_state;

typedef struct {
  proxy_body_kind kind;
  size_t remaining;
  int complete;
  proxy_chunk_state chunk_state;
  char line[HTTP_MAX_HEADER_BYTES];
  size_t line_length;
  int line_cr;
  size_t trailer_bytes;
  size_t trailer_count;
} proxy_body;

/* 성공 0, 실패 -1과 errno. */
int proxy_body_init(proxy_body *body, const proxy_response *response);

/* 받은 데이터 중 전달할 범위를 forward에 빌려 준다. 소비량은 forward->length다.
 * COMPLETE는 본문 완료, INCOMPLETE는 추가 수신 필요, INVALID는 잘못된 형식·EOF다.
 * chunk 크기 줄·트레일러의 한도 초과는 TOO_LARGE다. 청크 형식은 유지한다.
 * 오류 시 상태와 forward는 변경하지 않는다. 실제 송신·버퍼 복사·소켓 정리는 하지 않는다. */
http_parse_result proxy_body_consume(proxy_body *body, http_slice input, int eof,
                                       http_slice *forward);

#endif
