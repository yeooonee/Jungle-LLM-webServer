#ifndef PROXY_RELAY_H
#define PROXY_RELAY_H

#include <signal.h>
#include <time.h>

/* HTTP/1.1 클라이언트로 백엔드 응답 하나를 중계한다. 성공 0, 실패 -1과 errno.
 * timeout_ms는 헤더 수신부터 본문 송신까지의 전체 기한이다.
 * response_started는 초기 0, 클라이언트에 한 바이트라도 보냈으면 1이다.
 * 실패 시 이 값이 1이면 새 오류 응답을 보내면 안 된다.
 * 소켓·취소 fd는 호출자가 정리하며 SIGPIPE도 호출자가 무시해야 한다. */
int proxy_response_relay(int backend_fd, int client_fd, int timeout_ms,
                           const volatile sig_atomic_t *stopping, int cancel_fd,
                           int *response_started);

/* 요청 전체에서 공유하는 절대 기한을 받는 내부 진입점이다. */
int proxy_response_relay_until(int backend_fd, int client_fd,
                                 const struct timespec *deadline,
                                 const volatile sig_atomic_t *stopping, int cancel_fd,
                                 int *response_started);

#endif
