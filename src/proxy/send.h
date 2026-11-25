#ifndef PROXY_SEND_H
#define PROXY_SEND_H

#include "proxy/request.h"

#include <signal.h>
#include <time.h>

/* non-blocking backend_fd에 헤더·본문을 순서대로 보낸다. 성공 0, 실패 -1과 errno.
 * timeout_ms는 송신 전체의 기한이다. 호출자가 SIGPIPE를 무시해야 한다.
 * stopping=NULL, cancel_fd=-1이면 취소 감시를 사용하지 않는다.
 * cancel_fd는 취소 파이프의 읽기 끝이며 쓰기 끝을 닫아 취소한다.
 * 소켓·버퍼는 호출자 소유다. 실패 시 일부 송신되었을 수 있어 재전송하면 안 된다. */
int proxy_request_send(int backend_fd, const proxy_request *request, int timeout_ms,
                         const volatile sig_atomic_t *stopping, int cancel_fd);

/* 요청 전체에서 공유하는 절대 기한을 받는 내부 진입점이다. */
int proxy_request_send_until(int backend_fd, const proxy_request *request,
                               const struct timespec *deadline,
                               const volatile sig_atomic_t *stopping, int cancel_fd);

#endif
