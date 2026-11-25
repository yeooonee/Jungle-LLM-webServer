#include "proxy/io.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#include "proxy/deadline.h"

int proxy_io_wait(int fd, short events, const struct timespec *deadline,
                        const volatile sig_atomic_t *stopping, int cancel_fd) {
  for (;;) {
    if (stopping != NULL && *stopping) {
      errno = ECANCELED;
      return -1;
    }
    int timeout = proxy_deadline_remaining(deadline);
    if (timeout <= 0) {
      return -1;
    }
    struct pollfd pending[] = {
        {.fd = fd, .events = events},
        {.fd = cancel_fd, .events = POLLIN},
    };
    int ready = poll(pending, 2, timeout);
    if (ready == -1) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (ready == 0) {
      continue;
    }
    if ((stopping != NULL && *stopping) ||
        (pending[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      errno = ECANCELED;
      return -1;
    }
    if (pending[0].revents & POLLNVAL) {
      errno = EBADF;
      return -1;
    }
    if (pending[0].revents & (events | POLLHUP | POLLERR)) {
      return 0;
    }
  }
}

int proxy_io_send(int fd, http_slice data, const struct timespec *deadline,
                    const volatile sig_atomic_t *stopping, int cancel_fd, int *sent_any) {
  size_t sent = 0;
  while (sent < data.length) {
    if (proxy_io_wait(fd, POLLOUT, deadline, stopping, cancel_fd) == -1) {
      return -1;
    }
    ssize_t count = send(fd, data.data + sent, data.length - sent, MSG_DONTWAIT);
    if (count > 0) {
      sent += (size_t)count;
      if (sent_any != NULL) {
        *sent_any = 1;
      }
    } else if (count == 0) {
      errno = EIO;
      return -1;
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      return -1;
    }
  }
  return 0;
}

ssize_t proxy_io_recv(int fd, char *data, size_t capacity, const struct timespec *deadline,
                        const volatile sig_atomic_t *stopping, int cancel_fd) {
  for (;;) {
    if (proxy_io_wait(fd, POLLIN, deadline, stopping, cancel_fd) == -1) {
      return -1;
    }
    ssize_t count = recv(fd, data, capacity, MSG_DONTWAIT);
    if (count >= 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
      return count;
    }
  }
}
