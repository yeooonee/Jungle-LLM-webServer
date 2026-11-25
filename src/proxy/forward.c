#include "proxy/forward.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "http/response.h"
#include "proxy/connect.h"
#include "proxy/deadline.h"
#include "proxy/io.h"
#include "proxy/relay.h"
#include "proxy/request.h"
#include "proxy/send.h"

static int send_error(int client_fd, int status, int timeout_ms,
                        const volatile sig_atomic_t *stopping, int cancel_fd) {
  const char body[] = "Proxy request failed\n";
  http_response response = {status, "text/plain; charset=utf-8", body, sizeof(body) - 1};
  char headers[1024];
  int length = http_response_format_headers(&response, headers, sizeof(headers));
  if (length == -1) {
    return -1;
  }
  /* 백엔드 기한이 만료되어도 오류를 알리되, 느린 클라이언트 대기는 1초로 제한한다. */
  struct timespec deadline;
  if (proxy_deadline_start(&deadline, timeout_ms < 1000 ? timeout_ms : 1000) == -1 ||
      proxy_io_send(client_fd, (http_slice){headers, (size_t)length}, &deadline,
                       stopping, cancel_fd, NULL) == -1) {
    return -1;
  }
  return proxy_io_send(client_fd, (http_slice){body, sizeof(body) - 1}, &deadline,
                         stopping, cancel_fd, NULL);
}

int proxy_forward_request(int client_fd, const http_request *request,
                             const proxy_backend *backend, int timeout_ms,
                             const volatile sig_atomic_t *stopping, int cancel_fd) {
  /* shortcut: HTTP/1.1만 전달하며, HTTP/1.0 지원 시 chunked 변환 정책을 추가한다. */
  struct in_addr address;
  if (client_fd < 0 || request == NULL || backend == NULL || backend->ipv4 == NULL ||
      backend->port == 0 || inet_pton(AF_INET, backend->ipv4, &address) != 1 ||
      request->version.data == NULL || request->version.length != 8 ||
      memcmp(request->version.data, "HTTP/1.1", 8) != 0) {
    errno = EINVAL;
    return -1;
  }
  struct timespec deadline;
  if (proxy_deadline_start(&deadline, timeout_ms) == -1) {
    return -1;
  }
  if (stopping != NULL && *stopping) {
    errno = ECANCELED;
    return -1;
  }
  proxy_request outgoing;
  if (proxy_request_build(request, backend->ipv4, backend->port, &outgoing) == -1) {
    int status = errno == ENOTSUP ? 501 : errno == EINVAL ? 400 : errno == EMSGSIZE ? 431 : 500;
    return send_error(client_fd, status, timeout_ms, stopping, cancel_fd);
  }
  int started = 0;
  int backend_fd = proxy_connect_until(backend->ipv4, backend->port, &deadline,
                                         stopping, cancel_fd);
  int result = -1;
  if (backend_fd != -1 &&
      proxy_request_send_until(backend_fd, &outgoing, &deadline, stopping, cancel_fd) == 0) {
    result = proxy_response_relay_until(backend_fd, client_fd, &deadline, stopping,
                                          cancel_fd, &started);
  }
  int saved_error = errno;
  free(outgoing.headers);
  if (backend_fd != -1) {
    close(backend_fd);
  }
  if (result == 0) {
    return 0;
  }
  if (started || saved_error == ECANCELED) {
    errno = saved_error;
    return -1;
  }
  int status = saved_error == ETIMEDOUT ? 504 :
               saved_error == ENOMEM || saved_error == EMFILE || saved_error == ENFILE ? 500 : 502;
  return send_error(client_fd, status, timeout_ms, stopping, cancel_fd);
}
