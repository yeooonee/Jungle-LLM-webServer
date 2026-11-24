#ifndef NET_TCP_H
#define NET_TCP_H

#include <signal.h>
#include <stddef.h>

/* SIGPIPE 무시와 SO_SNDTIMEO 설정은 호출자가 담당한다. */
int tcp_send_all(int socket_fd, const void *data, size_t length,
                 const volatile sig_atomic_t *stopping);

int tcp_http_server(unsigned short port, const char *root, const char *cgi_program,
                    const volatile sig_atomic_t *stopping);

#endif
