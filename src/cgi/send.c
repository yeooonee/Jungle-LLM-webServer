#include "cgi/send.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cgi/run.h"
#include "http/response.h"

static int build_response(const cgi_response *response, http_response *http,
                            char content_type[HTTP_MAX_HEADER_BYTES + 1]) {
  if (response == NULL || response->content_type.data == NULL ||
      response->content_type.length == 0 ||
      (response->body.data == NULL && response->body.length != 0)) {
    errno = EINVAL;
    return -1;
  }
  if (response->content_type.length > HTTP_MAX_HEADER_BYTES ||
      response->body.length > HTTP_MAX_BODY_BYTES) {
    errno = EMSGSIZE;
    return -1;
  }
  if (memchr(response->content_type.data, '\0', response->content_type.length) != NULL) {
    errno = EINVAL;
    return -1;
  }

  /* HTTP 송신 모듈은 Content-Type을 NUL 종료 문자열로 받는다. */
  memcpy(content_type, response->content_type.data, response->content_type.length);
  content_type[response->content_type.length] = '\0';
  *http = (http_response){
      .status = response->status,
      .content_type = content_type,
      .body = response->body.data,
      .body_length = response->body.length,
  };
  return http_response_validate(http);
}

int cgi_response_send(int client_fd, const cgi_response *response,
                       const volatile sig_atomic_t *stopping) {
  char content_type[HTTP_MAX_HEADER_BYTES + 1];
  http_response http;
  if (build_response(response, &http, content_type) == -1) {
    return -1;
  }
  return http_response_send(client_fd, &http, stopping);
}

static int send_error(int client_fd, int status,
                       const volatile sig_atomic_t *stopping) {
  const char *body = status == 504 ? "Gateway Timeout\n" :
                     status == 502 ? "Bad Gateway\n" : "Internal Server Error\n";
  http_response error = {status, "text/plain; charset=utf-8", body, strlen(body)};
  return http_response_send(client_fd, &error, stopping);
}

int cgi_program_send(int client_fd, const char *program, http_slice body,
                      const volatile sig_atomic_t *stopping, int cancel_fd) {
  cgi_output output;
  int run_result = cgi_run(program, body, &output, stopping, cancel_fd);
  if (run_result != 0) {
    int error = errno;
    if (error == ECANCELED) {
      return -1;
    }
    int status = 500;
    if (error == ETIMEDOUT) {
      status = 504;
    } else if (run_result > 0 || error == EPROTO || error == EMSGSIZE || error == EPIPE) {
      status = 502;
    }
    return send_error(client_fd, status, stopping);
  }
  char content_type[HTTP_MAX_HEADER_BYTES + 1];
  http_response http;
  if (build_response(&output.response, &http, content_type) == -1) {
    free(output.data);
    return send_error(client_fd, 502, stopping);
  }
  /* 송신을 시작한 뒤 실패하면 두 번째 응답을 덧붙이지 않는다. */
  int result = http_response_send(client_fd, &http, stopping);
  int saved_error = errno;
  free(output.data);
  errno = saved_error;
  return result;
}
