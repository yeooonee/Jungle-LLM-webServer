#include "dispatch.h"

#include <stdio.h>
#include <unistd.h>

static int static_root_fd;
static const char *configured_cgi;
static const volatile sig_atomic_t *server_stopping;

int dispatch_start(int root_fd, const char *cgi_program,
                     const volatile sig_atomic_t *stopping) {
  static_root_fd = root_fd;
  configured_cgi = cgi_program;
  server_stopping = stopping;
  return 0;
}

void dispatch_client(int client_fd) {
  if (tcp_handle_client(client_fd, static_root_fd, configured_cgi, -1, server_stopping) == -1 &&
      !*server_stopping) {
    perror("client I/O");
  }
  close(client_fd);
}

void dispatch_stop(void) {
}
