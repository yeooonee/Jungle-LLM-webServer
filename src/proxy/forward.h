#ifndef PROXY_FORWARD_H
#define PROXY_FORWARD_H

#include "http/request.h"

#include <signal.h>

typedef struct {
  const char *ipv4;
  unsigned short port;
} proxy_backend;

/* 완전히 파싱된 HTTP/1.1 GET·POST 요청 하나를 지정한 백엔드로 전달한다.
 * timeout_ms는 연결·요청 송신·응답 중계의 공통 기한이다.
 * 성공 0에는 오류 HTTP 응답 송신도 포함한다. 실패 -1이면 호출자가 연결을 닫는다.
 * 백엔드 fd는 내부에서 닫으며 client_fd·요청·취소 fd는 호출자가 소유한다.
 * SIGPIPE는 호출자가 무시해야 한다. HTTP/1.0·잘못된 인자는 EINVAL로 거절한다. */
int proxy_forward_request(int client_fd, const http_request *request,
                             const proxy_backend *backend, int timeout_ms,
                             const volatile sig_atomic_t *stopping, int cancel_fd);

#endif
