#include "tcp.h"
#include "dispatch.h"

#include "dynamic/add.h"
#include "http/request.h"
#include "http/response.h"
#include "static/file.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int tcp_send_all(int socket_fd, const void *data, size_t length,
                 const volatile sig_atomic_t *stopping) {
  const char *buffer = data;
  size_t sent = 0;
  while (sent < length) {
    if (*stopping) {
      errno = ECANCELED;
      return -1;
    }
    ssize_t count = send(socket_fd, buffer + sent, length - sent, 0);
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
  return 0;
}

int tcp_handle_client(int client_fd, int root_fd,
                         const volatile sig_atomic_t *stopping) {
  struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
  if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) == -1) {
    return -1;
  }
  size_t capacity = HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_BYTES;
  char *buffer = malloc(capacity);
  if (buffer == NULL) {
    http_response response = {500, "text/plain", "Internal Server Error\n", 22};
    return http_response_send(client_fd, &response, stopping);
  }
  size_t used = 0;
  int result = 0;
  while (!*stopping) {
    ssize_t count = recv(client_fd, buffer + used, capacity - used, 0);
    if (count < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      result = -1;
      break;
    }
    if (count == 0 && used == 0)
      break;
    used += (size_t)count;
    http_request request;
    http_parse_result parsed = http_request_parse(buffer, used, &request);
    if (parsed == HTTP_PARSE_INCOMPLETE && count > 0 && used < capacity)
      continue;
    if (parsed == HTTP_PARSE_COMPLETE && request.method.length == 3 &&
        memcmp(request.method.data, "GET", 3) == 0) {
      result = static_file_send(client_fd, root_fd, request.target, stopping);
    } else if (parsed == HTTP_PARSE_COMPLETE && request.method.length == 4 &&
               memcmp(request.method.data, "POST", 4) == 0 &&
               request.target.length == 4 &&
               memcmp(request.target.data, "/add", 4) == 0) {
      /* `POST /add`의 경우에만 동적 처리로 넘김 */
      result = dynamic_add_send(client_fd, request.body, stopping);
    } else {
      int status = 400;
      const char *body = "Bad Request\n";
      if (parsed == HTTP_PARSE_TOO_LARGE ||
          (parsed == HTTP_PARSE_INCOMPLETE && used == capacity)) {
        status = 413;
        body = "Content Too Large\n";
      } else if (parsed == HTTP_PARSE_UNSUPPORTED ||
                 parsed == HTTP_PARSE_COMPLETE) {
        status = 501;
        body = "Not Implemented\n";
      }
      http_response response = {status, "text/plain; charset=utf-8", body,
                                strlen(body)};
      result = http_response_send(client_fd, &response, stopping);
    }
    break;
  }
  free(buffer);
  return result;
}

int tcp_http_server(unsigned short port, const char *root,
                    const volatile sig_atomic_t *stopping) {
  int root_fd = open(root, O_RDONLY | O_DIRECTORY);
  if (root_fd == -1) {
    perror("static root");
    return -1;
  }
  int listener_fd = socket(AF_INET, SOCK_STREAM,
                           0); /* IPv4, 연결 기반 바이트 스트림 소켓, TCP 사용*/
  if (listener_fd == -1) {
    perror("socket");
    close(root_fd);
    return -1;
  }

  int result = -1;
  int dispatcher_started = 0;
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

  if (dispatch_start(root_fd, stopping) == -1) {
    perror("dispatch_start");
    goto cleanup;
  }
  dispatcher_started = 1;

  printf("HTTP server listening on 0.0.0.0:%u\n", (unsigned int)port);
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
      close(client);
    } else {
      dispatch_client(client);
    }
  }
  result = 0;

cleanup:
  close(listener_fd);
  if (dispatcher_started) {
    dispatch_stop();
  }
  close(root_fd);
  return result;
}
