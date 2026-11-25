#ifndef PROXY_HEADERS_H
#define PROXY_HEADERS_H

#include "http/request.h"

/* 요청·응답 구성에서 공유하는 내부 헤더 처리 함수들이다. */
int proxy_header_name_is(http_slice name, const char *text);
int proxy_headers_filter(const http_header *headers, size_t count,
                           int removed[HTTP_MAX_HEADERS]);
int proxy_header_append(char *headers, size_t *used, const char *data, size_t length);

#endif
