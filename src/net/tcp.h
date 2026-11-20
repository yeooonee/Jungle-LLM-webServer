#ifndef NET_TCP_H
#define NET_TCP_H

#include <signal.h>

int tcp_echo_server(unsigned short port, const volatile sig_atomic_t *stopping);

#endif
