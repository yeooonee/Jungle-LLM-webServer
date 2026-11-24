#include "response.h"

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "net/tcp.h"

static const char *reason_phrase(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 413:
    return "Content Too Large";
  case 501:
    return "Not Implemented";
  case 502:
    return "Bad Gateway";
  case 504:
    return "Gateway Timeout";
  case 500:
    return "Internal Server Error";
  default:
    return NULL;
  }
}

static int valid_content_type(const char *value) {
  if (value == NULL || *value == '\0') {
    return 0;
  }
  for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
    /* 줄바꿈을 포함한 제어문자가 응답 헤더를 삽입하지 못하게 한다. */
    if (*p < 0x20 || *p > 0x7e) {
      return 0;
    }
  }
  return 1;
}

static int format_headers(const http_response *response, char *headers,
                           size_t capacity) {
  if (response == NULL || !valid_content_type(response->content_type)) {
    errno = EINVAL;
    return -1;
  }
  const char *reason = reason_phrase(response->status);
  if (reason == NULL) {
    errno = EINVAL;
    return -1;
  }
  /* shortcut: 연결당 응답 하나를 보내며, 연결 재사용 단계에서 정책을 확장한다. */
  int length = snprintf(headers, capacity,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        response->status, reason, response->content_type,
                        response->body_length);
  if (length < 0) {
    errno = EIO;
    return -1;
  }
  if ((size_t)length >= capacity) {
    errno = EMSGSIZE;
    return -1;
  }

  return length;
}

int http_response_validate(const http_response *response) {
  if (response == NULL || (response->body == NULL && response->body_length != 0)) {
    errno = EINVAL;
    return -1;
  }
  char headers[1024];
  return format_headers(response, headers, sizeof(headers)) < 0 ? -1 : 0;
}

int http_response_send_headers(int client_fd, const http_response *response,
                       const volatile sig_atomic_t *stopping) {
  if (stopping == NULL) {
    errno = EINVAL;
    return -1;
  }
  char headers[1024];
  int length = format_headers(response, headers, sizeof(headers));
  if (length < 0) {
    return -1;
  }
  if (*stopping) {
    errno = ECANCELED;
    return -1;
  }

  /* 시그널이 send 직전에 도착해도 종료 플래그를 다시 확인할 수 있게 한다. */
  struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
  if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1) {
    return -1;
  }
  return tcp_send_all(client_fd, headers, (size_t)length, stopping);
}

int http_response_send(int client_fd, const http_response *response,
                       const volatile sig_atomic_t *stopping) {
  if (response == NULL || (response->body == NULL && response->body_length != 0)) {
    errno = EINVAL;
    return -1;
  }
  if (http_response_send_headers(client_fd, response, stopping) == -1) {
    return -1;
  }
  return tcp_send_all(client_fd, response->body, response->body_length, stopping);
}
