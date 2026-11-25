#ifndef NET_TCP_H
#define NET_TCP_H

#include "proxy/route.h"

#include <signal.h>
#include <stddef.h>

/* SIGPIPE 무시와 SO_SNDTIMEO 설정은 호출자가 담당한다. */
int tcp_send_all(int socket_fd, const void *data, size_t length,
                 const volatile sig_atomic_t *stopping);

/* routes=NULL 또는 빈 목록이면 프록시를 끈다. 설정과 주소 문자열은 서버 종료까지 유지한다. */
int tcp_http_server(unsigned short port, const char *root, const char *cgi_program,
                    proxy_routes *routes,
                    const volatile sig_atomic_t *stopping);

#endif
