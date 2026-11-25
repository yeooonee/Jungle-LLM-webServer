#ifndef NET_DISPATCH_H
#define NET_DISPATCH_H

#include "proxy/route.h"

#include <signal.h>

#define THREAD_POOL_WORKERS 4
#define THREAD_POOL_QUEUE_CAPACITY 8

int tcp_handle_client(int client_fd, int root_fd, const char *cgi_program,
                       proxy_routes *routes, int cancel_fd,
                       const volatile sig_atomic_t *stopping);
/* routes는 빌려 쓰며 dispatch_stop까지 변경·해제하지 않는다. */
int dispatch_start(int root_fd, const char *cgi_program,
                     proxy_routes *routes,
                     const volatile sig_atomic_t *stopping);
/* 호출 후 연결 fd의 정리 책임은 선택된 실행 방식에 넘어간다. */
void dispatch_client(int client_fd);
void dispatch_stop(void);

#endif
