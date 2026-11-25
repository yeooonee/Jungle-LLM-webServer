#ifndef PROXY_IO_H
#define PROXY_IO_H

#include "http/request.h"

#include <signal.h>
#include <sys/types.h>
#include <time.h>

/* 내부 I/O 함수. 같은 기한·취소 조건으로 기다리며 fd와 버퍼를 정리하지 않는다.
 * 송신은 성공 0·실패 -1, 수신은 바이트 수·EOF 0·실패 -1과 errno다.
 * sent_any가 있으면 실제로 한 바이트라도 송신했을 때 1로 설정한다.
 * SIGPIPE 무시는 호출자 책임이다. */
int proxy_io_wait(int fd, short events, const struct timespec *deadline,
                     const volatile sig_atomic_t *stopping, int cancel_fd);
int proxy_io_send(int fd, http_slice data, const struct timespec *deadline,
                    const volatile sig_atomic_t *stopping, int cancel_fd, int *sent_any);
ssize_t proxy_io_recv(int fd, char *data, size_t capacity, const struct timespec *deadline,
                        const volatile sig_atomic_t *stopping, int cancel_fd);

#endif
