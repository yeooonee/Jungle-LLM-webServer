#ifndef PROXY_RESPONSE_HEADERS_H
#define PROXY_RESPONSE_HEADERS_H

#include "proxy/response.h"

typedef struct {
  char *data;
  size_t length;
} proxy_response_headers;

/* 헤더 파서가 완성한 최종 응답에서 HTTP/1.1 클라이언트용 헤더를 만든다. */
/* 성공 0: 호출자가 output->data를 free한다. 실패 -1과 errno: output은 변경하지 않는다. */
int proxy_response_build_headers(const proxy_response *response,
                                    proxy_response_headers *output);

#endif
