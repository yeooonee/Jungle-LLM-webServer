#ifndef PROXY_CONNECT_H
#define PROXY_CONNECT_H

#include <signal.h>
#include <time.h>

/* IPv4 숫자 주소에 timeout_ms(양수) 이내로 연결한다.
 * 성공: non-blocking·close-on-exec fd 반환. 호출자가 close한다.
 * 실패: -1과 errno. 시간 초과는 ETIMEDOUT이며 내부 fd는 닫는다. */
int proxy_connect(const char *ipv4, unsigned short port, int timeout_ms);

/* 요청 전체에서 공유하는 절대 기한을 받는 내부 진입점이다. */
int proxy_connect_until(const char *ipv4, unsigned short port,
                          const struct timespec *deadline,
                          const volatile sig_atomic_t *stopping, int cancel_fd);

#endif
