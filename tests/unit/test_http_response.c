#include "http/response.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;

static void check_response(int status, const char *reason,
                           const void *body, size_t body_length) {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  http_response response = {status, "application/octet-stream", body, body_length};
  assert(http_response_send(sockets[0], &response, &stopping) == 0);
  assert(shutdown(sockets[0], SHUT_WR) == 0);

  char actual[2048];
  size_t received = 0;
  ssize_t count;
  while ((count = recv(sockets[1], actual + received, sizeof(actual) - received, 0)) > 0) {
    received += (size_t)count;
    assert(received < sizeof(actual));
  }
  assert(count == 0);
  char expected[1024];
  int header_length = snprintf(expected, sizeof(expected),
      "HTTP/1.1 %d %s\r\nContent-Type: application/octet-stream\r\n"
      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
      status, reason, body_length);
  assert(header_length > 0 && (size_t)header_length < sizeof(expected));
  assert(received == (size_t)header_length + body_length);
  assert(memcmp(actual, expected, (size_t)header_length) == 0);
  if (body_length != 0) {
    assert(memcmp(actual + header_length, body, body_length) == 0);
  }
  close(sockets[0]);
  close(sockets[1]);
}

static void check_invalid(const http_response *response, int error) {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(http_response_send(sockets[0], response, &stopping) == -1);
  assert(errno == error);
  char byte;
  assert(recv(sockets[1], &byte, 1, MSG_DONTWAIT) == -1);
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  close(sockets[0]);
  close(sockets[1]);
}

struct reader_args {
  int socket_fd;
  char *data;
  size_t length;
  size_t capacity;
};

static void *read_slowly(void *arg) {
  struct reader_args *reader = arg;
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 350000000};
  assert(nanosleep(&delay, NULL) == 0);
  ssize_t count;
  while ((count = recv(reader->socket_fd, reader->data + reader->length,
                       reader->capacity - reader->length, 0)) > 0) {
    reader->length += (size_t)count;
    assert(reader->length < reader->capacity);
  }
  assert(count == 0);
  return NULL;
}

static void check_large_body(void) {
  size_t length = 2 * 1024 * 1024;
  char *body = malloc(length);
  char *received = malloc(length + 1024);
  assert(body != NULL && received != NULL);
  for (size_t i = 0; i < length; ++i) {
    body[i] = (char)(i % 256);
  }
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  int send_buffer = 1024;
  assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
  struct reader_args reader = {sockets[1], received, 0, length + 1024};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, read_slowly, &reader) == 0);
  http_response response = {200, "application/octet-stream", body, length};
  assert(http_response_send(sockets[0], &response, &stopping) == 0);
  assert(shutdown(sockets[0], SHUT_WR) == 0);
  assert(pthread_join(thread, NULL) == 0);
  const char *expected = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                         "Content-Length: 2097152\r\nConnection: close\r\n\r\n";
  size_t header_length = strlen(expected);
  assert(reader.length == header_length + length);
  assert(memcmp(received, expected, header_length) == 0);
  assert(memcmp(received + header_length, body, length) == 0);
  close(sockets[0]);
  close(sockets[1]);
  free(body);
  free(received);
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGPIPE, &action, NULL) == 0);

  check_response(200, "OK", "hello", 5);
  check_response(400, "Bad Request", "bad request", 11);
  check_response(403, "Forbidden", "forbidden", 9);
  check_response(404, "Not Found", "not found", 9);
  check_response(431, "Request Header Fields Too Large", "headers", 7);
  check_response(500, "Internal Server Error", "error", 5);
  check_response(502, "Bad Gateway", "bad gateway", 11);
  check_response(504, "Gateway Timeout", "timeout", 7);
  check_response(505, "HTTP Version Not Supported", "version", 7);
  check_response(200, "OK", NULL, 0);
  const char binary[] = {'a', '\0', '\xff', '\r', '\n'};
  check_response(200, "OK", binary, sizeof(binary));
  check_response(200, "OK", "한글", sizeof("한글") - 1);

  check_invalid(NULL, EINVAL);
  http_response invalid[] = {
      {999, "text/plain", NULL, 0},
      {200, NULL, NULL, 0},
      {200, "", NULL, 0},
      {200, "text/plain\r\nX: injected", NULL, 0},
      {200, "text/plain\nX: injected", NULL, 0},
      {200, "text/\x01plain", NULL, 0},
      {200, "text/plain", NULL, 1},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    check_invalid(&invalid[i], EINVAL);
  }
  char long_type[1024];
  memset(long_type, 'a', sizeof(long_type) - 1);
  long_type[sizeof(long_type) - 1] = '\0';
  http_response oversized = {200, long_type, NULL, 0};
  check_invalid(&oversized, EMSGSIZE);

  stopping = 1;
  http_response response = {200, "text/plain", NULL, 0};
  char formatted[256];
  int formatted_length = http_response_format_headers(&response, formatted, sizeof(formatted));
  assert(formatted_length > 0 && (size_t)formatted_length == strlen(formatted));
  assert(strstr(formatted, "Content-Length: 0\r\n") != NULL);
  assert(http_response_format_headers(&response, NULL, 0) == -1 && errno == EINVAL);
  assert(http_response_format_headers(&response, formatted, 1) == -1 && errno == EMSGSIZE);
  check_invalid(&response, ECANCELED);
  stopping = 0;

  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(http_response_send(sockets[0], &response, NULL) == -1 && errno == EINVAL);
  close(sockets[1]);
  assert(http_response_send(sockets[0], &response, &stopping) == -1);
  /* macOS에서는 끊어진 소켓의 setsockopt가 먼저 EINVAL로 실패할 수 있다. */
  assert(errno == EPIPE || errno == ECONNRESET || errno == EINVAL);
  close(sockets[0]);

  check_large_body();
  puts("HTTP response checks passed");
  return 0;
}
