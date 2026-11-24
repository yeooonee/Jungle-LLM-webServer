#include "cgi/send.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;

static void check_send(const char *cgi, size_t length, const char *expected,
                        size_t expected_length) {
  cgi_response response;
  assert(cgi_response_parse(cgi, length, &response) == 1);
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(cgi_response_send(sockets[0], &response, &stopping) == 0);
  assert(shutdown(sockets[0], SHUT_WR) == 0);
  char actual[1024];
  size_t received = 0;
  ssize_t count;
  while ((count = recv(sockets[1], actual + received, sizeof(actual) - received, 0)) > 0) {
    received += (size_t)count;
    assert(received < sizeof(actual));
  }
  assert(count == 0 && received == expected_length);
  assert(memcmp(actual, expected, expected_length) == 0);
  close(sockets[0]);
  close(sockets[1]);
}

static void check_invalid(const cgi_response *response, int error) {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(cgi_response_send(sockets[0], response, &stopping) == -1);
  assert(errno == error);
  char byte;
  assert(recv(sockets[1], &byte, 1, MSG_DONTWAIT) == -1);
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  close(sockets[0]);
  close(sockets[1]);
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = SIG_IGN;
  assert(sigemptyset(&action.sa_mask) == 0);
  assert(sigaction(SIGPIPE, &action, NULL) == 0);

  const char cgi[] = "Content-Type: application/octet-stream\r\n"
                     "Content-Length: 999\r\nConnection: keep-alive\r\n"
                     "X-Note: ignored\r\n\r\nA\0B";
  const char http[] = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                      "Content-Length: 3\r\nConnection: close\r\n\r\nA\0B";
  check_send(cgi, sizeof(cgi) - 1, http, sizeof(http) - 1);

  const char bad[] = "Status: 400 Custom reason\r\nContent-Type: application/json\r\n\r\n{}";
  const char bad_http[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                          "Content-Length: 2\r\nConnection: close\r\n\r\n{}";
  check_send(bad, sizeof(bad) - 1, bad_http, sizeof(bad_http) - 1);
  const char empty[] = "Status: 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\n";
  const char empty_http[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n";
  check_send(empty, sizeof(empty) - 1, empty_http, sizeof(empty_http) - 1);

  check_invalid(NULL, EINVAL);
  cgi_response response = {200, {"text/plain", 10}, {NULL, 0}};
  stopping = 1;
  check_invalid(&response, ECANCELED);
  stopping = 0;
  response.status = 201;
  check_invalid(&response, EINVAL);
  response.status = 200;
  response.content_type = (http_slice){"text/plain\0extra", 16};
  check_invalid(&response, EINVAL);
  response.content_type = (http_slice){"text/plain\r\nX: injected", 23};
  check_invalid(&response, EINVAL);
  response.content_type = (http_slice){NULL, 0};
  check_invalid(&response, EINVAL);
  response.content_type = (http_slice){"text/plain", 10};
  response.body = (http_slice){NULL, 1};
  check_invalid(&response, EINVAL);
  response.body = (http_slice){"", HTTP_MAX_BODY_BYTES + 1};
  check_invalid(&response, EMSGSIZE);
  response.body = (http_slice){NULL, 0};
  char long_type[1024];
  memset(long_type, 'a', sizeof(long_type));
  response.content_type = (http_slice){long_type, sizeof(long_type)};
  check_invalid(&response, EMSGSIZE);
  puts("CGI HTTP send checks passed");
  return 0;
}
