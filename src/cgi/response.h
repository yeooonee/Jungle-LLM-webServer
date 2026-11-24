#ifndef CGI_RESPONSE_H
#define CGI_RESPONSE_H

#include "http/request.h"

typedef struct {
  int status;
  http_slice content_type;
  http_slice body;
} cgi_response;

/* EOF까지 받은 출력 전체를 해석한다. 성공 1, 실패 0. 결과는 원본을 빌려 쓴다. */
int cgi_response_parse(const char *data, size_t length, cgi_response *response);

#endif
