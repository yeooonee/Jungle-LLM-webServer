#include "proxy/send.h"

#include <errno.h>
#include <fcntl.h>

#include "proxy/deadline.h"
#include "proxy/io.h"

int proxy_request_send(int backend_fd, const proxy_request *request, int timeout_ms,
                         const volatile sig_atomic_t *stopping, int cancel_fd) {
  struct timespec deadline;
  if (proxy_deadline_start(&deadline, timeout_ms) == -1) {
    return -1;
  }
  return proxy_request_send_until(backend_fd, request, &deadline, stopping, cancel_fd);
}

int proxy_request_send_until(int backend_fd, const proxy_request *request,
                               const struct timespec *deadline,
                               const volatile sig_atomic_t *stopping, int cancel_fd) {
  if (deadline == NULL || request == NULL || request->headers == NULL || request->headers_length == 0 ||
      request->headers_length > HTTP_MAX_HEADER_BYTES ||
      request->body.length > HTTP_MAX_BODY_BYTES ||
      (request->body.data == NULL && request->body.length != 0)) {
    errno = EINVAL;
    return -1;
  }
  int flags = fcntl(backend_fd, F_GETFL);
  if (flags == -1) {
    return -1;
  }
  if (!(flags & O_NONBLOCK)) {
    errno = EINVAL;
    return -1;
  }
  http_slice headers = {request->headers, request->headers_length};
  if (proxy_io_send(backend_fd, headers, deadline, stopping, cancel_fd, NULL) == -1) {
    return -1;
  }
  return proxy_io_send(backend_fd, request->body, deadline, stopping, cancel_fd, NULL);
}
