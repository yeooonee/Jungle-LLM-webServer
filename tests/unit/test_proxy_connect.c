#include "proxy/connect.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  assert(proxy_connect(NULL, 8001, 1000) == -1 && errno == EINVAL);
  assert(proxy_connect("localhost", 8001, 1000) == -1 && errno == EINVAL);
  assert(proxy_connect("999.0.0.1", 8001, 1000) == -1 && errno == EINVAL);
  assert(proxy_connect("127.0.0.1", 0, 1000) == -1 && errno == EINVAL);
  assert(proxy_connect("127.0.0.1", 8001, 0) == -1 && errno == EINVAL);
  assert(proxy_connect("127.0.0.1", 8001, -1) == -1 && errno == EINVAL);

  int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener_fd != -1);
  struct sockaddr_in address = {
      .sin_family = AF_INET, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
  assert(bind(listener_fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(listener_fd, (struct sockaddr *)&address, &length) == 0);
  unsigned short port = ntohs(address.sin_port);
  assert(listen(listener_fd, 4) == 0);

  for (int i = 0; i < 16; ++i) {
    int backend_fd = proxy_connect("127.0.0.1", port, 1000);
    assert(backend_fd != -1);
    int flags = fcntl(backend_fd, F_GETFL);
    assert(flags != -1 && (flags & O_NONBLOCK));
    flags = fcntl(backend_fd, F_GETFD);
    assert(flags != -1 && (flags & FD_CLOEXEC));
    struct pollfd pending = {.fd = listener_fd, .events = POLLIN};
    assert(poll(&pending, 1, 1000) == 1 && (pending.revents & POLLIN));
    int accepted_fd = accept(listener_fd, NULL, NULL);
    assert(accepted_fd != -1);
    assert(send(backend_fd, "hello", 5, 0) == 5);
    char body[5];
    size_t received = 0;
    while (received < sizeof(body)) {
      ssize_t count = recv(accepted_fd, body + received, sizeof(body) - received, 0);
      assert(count > 0);
      received += (size_t)count;
    }
    assert(memcmp(body, "hello", sizeof(body)) == 0);
    assert(close(backend_fd) == 0 && close(accepted_fd) == 0);
  }
  assert(close(listener_fd) == 0);
  int reserved_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(reserved_fd != -1);
  address.sin_port = 0;
  assert(bind(reserved_fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
  assert(getsockname(reserved_fd, (struct sockaddr *)&address, &length) == 0);
  port = ntohs(address.sin_port);
  struct timespec started;
  struct timespec finished;
  assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
  assert(proxy_connect("127.0.0.1", port, 50) == -1);
  int pending_error = errno;
  assert(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
  /* 바인딩만 한 포트는 OS에 따라 즉시 거절되거나 연결 대기가 만료된다. */
  assert(pending_error == ETIMEDOUT || pending_error == ECONNREFUSED);
  double elapsed = (double)(finished.tv_sec - started.tv_sec) +
                   (finished.tv_nsec - started.tv_nsec) / 1000000000.0;
  assert(elapsed < 1.0);
  if (pending_error == ETIMEDOUT) {
    assert(elapsed >= 0.045);
  }
  assert(close(reserved_fd) == 0);
  int next_fd = dup(STDIN_FILENO);
  assert(next_fd != -1 && close(next_fd) == 0);
  for (int i = 0; i < 16; ++i) {
    assert(proxy_connect("127.0.0.1", port, 1000) == -1);
    assert(errno == ECONNREFUSED);
  }
  int after_failure = dup(STDIN_FILENO);
  assert(after_failure == next_fd);
  assert(close(after_failure) == 0);
  printf("Proxy backend connection checks passed (%s)\n",
         pending_error == ETIMEDOUT ? "deadline timeout observed" : "refused before deadline");
  return 0;
}
