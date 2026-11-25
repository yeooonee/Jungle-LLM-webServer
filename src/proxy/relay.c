#include "proxy/relay.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "proxy/body.h"
#include "proxy/deadline.h"
#include "proxy/io.h"
#include "proxy/response_headers.h"

#define PROXY_MAX_INTERIM_RESPONSES 16

static int read_headers(int backend_fd, char *buffer, size_t *used,
                          proxy_response *response, const struct timespec *deadline,
                          const volatile sig_atomic_t *stopping, int cancel_fd) {
  size_t interim_count = 0;
  for (;;) {
    http_parse_result parsed = proxy_response_parse_headers(buffer, *used, response);
    if (parsed == HTTP_PARSE_COMPLETE) {
      if (response->status >= 200) {
        return 0;
      }
      if (++interim_count > PROXY_MAX_INTERIM_RESPONSES) {
        errno = EPROTO;
        return -1;
      }
      *used -= response->consumed;
      memmove(buffer, buffer + response->consumed, *used);
      continue;
    }
    if (parsed != HTTP_PARSE_INCOMPLETE) {
      errno = parsed == HTTP_PARSE_TOO_LARGE ? EMSGSIZE : EPROTO;
      return -1;
    }
    if (*used == HTTP_MAX_HEADER_BYTES) {
      errno = EMSGSIZE;
      return -1;
    }
    ssize_t count = proxy_io_recv(backend_fd, buffer + *used, HTTP_MAX_HEADER_BYTES - *used,
                                    deadline, stopping, cancel_fd);
    if (count <= 0) {
      if (count == 0) {
        errno = EPROTO;
      }
      return -1;
    }
    *used += (size_t)count;
  }
}

int proxy_response_relay(int backend_fd, int client_fd, int timeout_ms,
                           const volatile sig_atomic_t *stopping, int cancel_fd,
                           int *response_started) {
  if (response_started != NULL) {
    *response_started = 0;
  }
  struct timespec deadline;
  if (proxy_deadline_start(&deadline, timeout_ms) == -1) {
    return -1;
  }
  return proxy_response_relay_until(backend_fd, client_fd, &deadline, stopping,
                                     cancel_fd, response_started);
}

int proxy_response_relay_until(int backend_fd, int client_fd,
                                 const struct timespec *deadline,
                                 const volatile sig_atomic_t *stopping, int cancel_fd,
                                 int *response_started) {
  if (response_started == NULL) {
    errno = EINVAL;
    return -1;
  }
  *response_started = 0;
  if (deadline == NULL || backend_fd < 0 || client_fd < 0 || backend_fd == client_fd) {
    errno = EINVAL;
    return -1;
  }
  char buffer[16384];
  size_t used = 0;
  proxy_response response;
  if (read_headers(backend_fd, buffer, &used, &response, deadline, stopping, cancel_fd) == -1) {
    return -1;
  }
  proxy_body body;
  if (proxy_body_init(&body, &response) == -1) {
    return -1;
  }
  proxy_response_headers outgoing;
  if (proxy_response_build_headers(&response, &outgoing) == -1) {
    if (errno == EINVAL || errno == ENOTSUP) {
      errno = EPROTO;
    }
    return -1;
  }
  int result = proxy_io_send(client_fd, (http_slice){outgoing.data, outgoing.length},
                               deadline, stopping, cancel_fd, response_started);
  int saved_error = errno;
  free(outgoing.data);
  if (result == -1) {
    errno = saved_error;
    return -1;
  }
  http_slice input = {buffer + response.consumed, used - response.consumed};
  int eof = 0;
  for (;;) {
    http_slice forward;
    http_parse_result parsed = proxy_body_consume(&body, input, eof, &forward);
    if (parsed != HTTP_PARSE_COMPLETE && parsed != HTTP_PARSE_INCOMPLETE) {
      errno = parsed == HTTP_PARSE_TOO_LARGE ? EMSGSIZE : EPROTO;
      return -1;
    }
    if (proxy_io_send(client_fd, forward, deadline, stopping, cancel_fd, response_started) == -1) {
      return -1;
    }
    if (parsed == HTTP_PARSE_COMPLETE) {
      return 0;
    }
    ssize_t count = proxy_io_recv(backend_fd, buffer, sizeof(buffer), deadline, stopping, cancel_fd);
    if (count == -1) {
      return -1;
    }
    input = (http_slice){buffer, (size_t)count};
    eof = count == 0;
  }
}
