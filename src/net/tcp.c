#include "tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <sys/_endian.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int echo_client(int client, const volatile sig_atomic_t *stopping) {
  /* I/O 직전에 종료 시그널이 도착해도 무한히 기다리지 않도록 대기 시간을 제한.
   */
  struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
  if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          -1 ||
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
          -1) {
    return -1;
  }
  char buffer[4096];
  while (!*stopping) {
    ssize_t received = recv(client, buffer, sizeof(buffer), 0);
    if (received == 0) {
      return 0;
    }
    if (received < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return -1;
    }
    size_t sent = 0;
    while (sent < (size_t)received && !*stopping) {
      ssize_t count = send(client, buffer + sent, (size_t)received - sent, 0);
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        return -1;
      }
      if (count == 0) {
        errno = EIO;
        return -1;
      }
      sent += (size_t)count;
    }
  }
  return 0;
}

int tcp_echo_server(unsigned short port,
                    const volatile sig_atomic_t *stopping) {
  int listener_fd = socket(AF_INET, SOCK_STREAM,
                           0); /* IPv4, 연결 기반 바이트 스트림 소켓, TCP 사용*/
  if (listener_fd == -1) {
    perror("socket");
    return -1;
  }

  int result = -1;
  int reuse = 1;
  if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) == -1) {
    perror("setsockopt SO_REUSEADDR");
    goto cleanup;
  }

  /* 사용할 주소 구성 */
  struct sockaddr_in address = {
      .sin_family = AF_INET,   /* IPv4 주소 */
      .sin_port = htons(port), /* 사용할 포트 */
      .sin_addr = {.s_addr = htonl(INADDR_ANY)},
  };

  if (bind(listener_fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
    perror("bind");
    goto cleanup;
  }

  if (listen(listener_fd, 16) == -1) {
    perror("listen");
    goto cleanup;
  }

  /* poll과 accept 사이에 대기 중인 연결이 사라질 수 있음. */
  int flags = fcntl(listener_fd, F_GETFL);
  if (flags == -1 || fcntl(listener_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("fcntl listener");
    goto cleanup;
  }

  printf("TCP echo server listening on 0.0.0.0:%u\n", (unsigned int)port);
  fflush(stdout);

  while (!*stopping) {
    struct pollfd pending = {.fd = listener_fd, .events = POLLIN};
    int ready = poll(&pending, 1, 250);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      goto cleanup;
    }
    if (ready == 0 || *stopping) {
      continue;
    }
    if (!(pending.revents & POLLIN)) {
      fprintf(stderr, "Listener became unavailable\n");
      goto cleanup;
    }
    int client = accept(listener_fd, NULL, NULL);
    if (client == -1) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == ECONNABORTED) {
        continue;
      }
      perror("accept");
      goto cleanup;
    }
    /* 일부 플랫폼에서는 연결 소켓이 리스닝 소켓의 O_NONBLOCK 설정을 상속. */
    flags = fcntl(client, F_GETFL);
    if (flags == -1 || fcntl(client, F_SETFL, flags & ~O_NONBLOCK) == -1) {
      perror("fcntl client");
    } else if (echo_client(client, stopping) == -1 && !*stopping) {
      perror("client I/O");
    }
    close(client);
  }
  result = 0;

cleanup:
  close(listener_fd);
  return result;
}
