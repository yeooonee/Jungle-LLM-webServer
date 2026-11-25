#include "proxy/connect.h"
#include "proxy/deadline.h"
#include "proxy/io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int proxy_connect(const char *ipv4, unsigned short port, int timeout_ms) {
  struct timespec deadline;
  if (proxy_deadline_start(&deadline, timeout_ms) == -1) {
    return -1;
  }
  return proxy_connect_until(ipv4, port, &deadline, NULL, -1);
}

int proxy_connect_until(const char *ipv4, unsigned short port,
                          const struct timespec *deadline,
                          const volatile sig_atomic_t *stopping, int cancel_fd) {
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (ipv4 == NULL || port == 0 || deadline == NULL ||
      inet_pton(AF_INET, ipv4, &address.sin_addr) != 1) {
    errno = EINVAL;
    return -1;
  }
  if (stopping != NULL && *stopping) {
    errno = ECANCELED;
    return -1;
  }
  if (proxy_deadline_remaining(deadline) <= 0) {
    return -1;
  }
  int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (backend_fd == -1) {
    return -1;
  }
  int saved_error;
  int flags = fcntl(backend_fd, F_GETFL);
  if (flags == -1 || fcntl(backend_fd, F_SETFL, flags | O_NONBLOCK) == -1 ||
      fcntl(backend_fd, F_SETFD, FD_CLOEXEC) == -1) {
    goto fail;
  }
  if (connect(backend_fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
    return backend_fd;
  }
  if (errno != EINPROGRESS && errno != EINTR) {
    goto fail;
  }
  {
    if (proxy_io_wait(backend_fd, POLLOUT, deadline, stopping, cancel_fd) == -1) {
      goto fail;
    }
    int error;
    socklen_t error_length = sizeof(error);
    if (getsockopt(backend_fd, SOL_SOCKET, SO_ERROR, &error, &error_length) == -1) {
      goto fail;
    }
    if (error != 0) {
      errno = error;
      goto fail;
    }
    return backend_fd;
  }
fail:
  saved_error = errno;
  close(backend_fd);
  errno = saved_error;
  return -1;
}
