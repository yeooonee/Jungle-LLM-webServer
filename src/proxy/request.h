#ifndef PROXY_REQUEST_H
#define PROXY_REQUEST_H

#include "http/request.h"

typedef struct {
  char *headers;
  size_t headers_length;
  http_slice body;
} proxy_request;

/* HTTP_PARSE_COMPLETE인 GET·POST 요청을 받는다. 성공 0, 실패 -1과 errno.
 * 성공 시 headers는 호출자가 free하며 body는 원본 요청 버퍼를 빌려 쓴다.
 * 실패 시 output은 변경하지 않는다. 백엔드는 IPv4 숫자 주소만 허용한다. */
int proxy_request_build(const http_request *request, const char *backend_ipv4,
                          unsigned short port, proxy_request *output);

#endif
