#include "proxy/relay.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct writer_args {
  int fd;
  const char *data;
  size_t length;
  int eof;
  int fragmented;
  int completed;
};

static void *write_backend(void *argument) {
  struct writer_args *args = argument;
  size_t sent = 0;
  while (sent < args->length) {
    size_t amount = args->fragmented ? 1 : args->length - sent;
    ssize_t count = send(args->fd, args->data + sent, amount, 0);
    if (count == -1 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return NULL;
    }
    sent += (size_t)count;
    if (args->fragmented) {
      struct timespec delay = {.tv_nsec = 1000000};
      assert(nanosleep(&delay, NULL) == 0);
    }
  }
  args->completed = 1;
  if (args->eof) {
    assert(shutdown(args->fd, SHUT_WR) == 0);
  }
  return NULL;
}

static size_t read_all(int fd, char *data, size_t capacity) {
  size_t used = 0;
  for (;;) {
    struct pollfd pending = {.fd = fd, .events = POLLIN};
    assert(poll(&pending, 1, 3000) > 0);
    assert(used < capacity);
    ssize_t count = recv(fd, data + used, capacity - used, 0);
    assert(count >= 0);
    if (count == 0) {
      return used;
    }
    used += (size_t)count;
  }
}

struct relay_result {
  int result;
  int error;
  int started;
  char data[4096];
  size_t length;
};

static struct relay_result relay_case(const char *wire, size_t length, int eof,
                                       int fragmented) {
  int backend[2];
  int client[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, backend) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
  int original_flags = fcntl(client[0], F_GETFL);
  assert(original_flags != -1);
  struct writer_args writer = {backend[1], wire, length, eof, fragmented, 0};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, write_backend, &writer) == 0);
  struct relay_result output = {0};
  output.result = proxy_response_relay(backend[0], client[0], fragmented ? 1000 : 100,
                                        NULL, -1, &output.started);
  output.error = errno;
  assert(fcntl(client[0], F_GETFL) == original_flags && fcntl(backend[0], F_GETFL) != -1);
  close(backend[0]);
  close(client[0]);
  assert(pthread_join(thread, NULL) == 0);
  close(backend[1]);
  output.length = read_all(client[1], output.data, sizeof(output.data) - 1);
  output.data[output.length] = '\0';
  close(client[1]);
  return output;
}

static void expect(const char *wire, int eof, const char *expected) {
  struct relay_result output = relay_case(wire, strlen(wire), eof, 0);
  assert(output.result == 0 && output.started == 1);
  assert(output.length == strlen(expected));
  assert(memcmp(output.data, expected, output.length) == 0);
}

struct reader_args {
  int fd;
  char *data;
  size_t capacity;
  size_t received;
};

static void *read_slowly(void *argument) {
  struct reader_args *args = argument;
  struct timespec delay = {.tv_nsec = 100000000};
  assert(nanosleep(&delay, NULL) == 0);
  args->received = read_all(args->fd, args->data, args->capacity);
  return NULL;
}

static void *cancel_relay(void *argument) {
  int fd = *(int *)argument;
  struct timespec delay = {.tv_nsec = 50000000};
  assert(nanosleep(&delay, NULL) == 0);
  assert(close(fd) == 0);
  return NULL;
}

static void check_large(int mode) {
  size_t body_length = 2 * 1024 * 1024;
  char *wire = malloc(body_length + 256);
  char *received = malloc(body_length + 1024);
  assert(wire != NULL && received != NULL);
  int header_length = snprintf(wire, 256,
                                "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                                "Content-Type: application/octet-stream\r\n\r\n", body_length);
  assert(header_length > 0 && header_length < 256);
  for (size_t i = 0; i < body_length; ++i) {
    wire[(size_t)header_length + i] = (char)(i % 256);
  }
  int backend[2];
  int client[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, backend) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
  int small_buffer = 1024;
  assert(setsockopt(client[0], SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)) == 0);
  struct writer_args writer = {backend[1], wire, (size_t)header_length + body_length, 0, 0, 0};
  struct reader_args reader = {client[1], received, body_length + 1024, 0};
  pthread_t sender;
  pthread_t receiver;
  pthread_t canceller;
  int cancel_pipe[2] = {-1, -1};
  assert(pthread_create(&sender, NULL, write_backend, &writer) == 0);
  if (mode == 0) {
    assert(pthread_create(&receiver, NULL, read_slowly, &reader) == 0);
  } else if (mode == 2) {
    assert(pipe(cancel_pipe) == 0);
    assert(pthread_create(&canceller, NULL, cancel_relay, &cancel_pipe[1]) == 0);
  }
  int started = 0;
  int result = proxy_response_relay(backend[0], client[0], mode == 1 ? 100 : 2000,
                                     NULL, cancel_pipe[0], &started);
  int error = errno;
  assert(started == 1 && fcntl(client[0], F_GETFL) != -1);
  close(backend[0]);
  close(client[0]);
  assert(pthread_join(sender, NULL) == 0);
  close(backend[1]);
  if (mode == 0) {
    assert(result == 0 && writer.completed);
    assert(pthread_join(receiver, NULL) == 0);
    const char expected[] = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2097152\r\n"
                            "Content-Type: application/octet-stream\r\n\r\n";
    assert(reader.received == sizeof(expected) - 1 + body_length);
    assert(memcmp(received, expected, sizeof(expected) - 1) == 0);
    assert(memcmp(received + sizeof(expected) - 1, wire + header_length, body_length) == 0);
  } else if (mode == 1) {
    assert(result == -1 && error == ETIMEDOUT);
  } else {
    assert(result == -1 && error == ECANCELED);
    assert(pthread_join(canceller, NULL) == 0);
    assert(fcntl(cancel_pipe[0], F_GETFD) != -1);
    close(cancel_pipe[0]);
  }
  close(client[1]);
  free(wire);
  free(received);
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = SIG_IGN;
  assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGPIPE, &action, NULL) == 0);
  expect("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA", 0,
         "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello");
  expect("HTTP/1.0 200 OK\r\n\r\nhello", 1,
         "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello");
  expect("HTTP/1.1 204 No Content\r\n\r\nignored", 0,
         "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
  expect("HTTP/1.1 304 Not Modified\r\nContent-Length: 99\r\n\r\nignored", 0,
         "HTTP/1.1 304 Not Modified\r\nConnection: close\r\nContent-Length: 99\r\n\r\n");
  expect("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0,
         "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
         "5\r\nhello\r\n0\r\nX-Checksum: abc\r\n\r\nEXTRA", 0,
         "HTTP/1.1 200 OK\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n"
         "5\r\nhello\r\n0\r\nX-Checksum: abc\r\n\r\n");

  const char fragmented[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  struct relay_result output = relay_case(fragmented, sizeof(fragmented) - 1, 0, 1);
  assert(output.result == 0 && strstr(output.data, "\r\n\r\nhello") != NULL);
  const char *invalid[] = {
      "", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n",
      "HTTP/1.1 101 Switching Protocols\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
      "HTTP/1.1 200 OK\r\nConnection: X-One X-Two\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    output = relay_case(invalid[i], strlen(invalid[i]), 1, 0);
    assert(output.result == -1 && output.error == EPROTO && !output.started && output.length == 0);
  }
  const char short_body[] = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
  output = relay_case(short_body, sizeof(short_body) - 1, 1, 0);
  assert(output.result == -1 && output.error == EPROTO && output.started);
  assert(strncmp(output.data, "HTTP/1.1 200 OK", 15) == 0 && strstr(output.data, "502") == NULL);
  const char short_chunk[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhi";
  output = relay_case(short_chunk, sizeof(short_chunk) - 1, 1, 0);
  assert(output.result == -1 && output.error == EPROTO && output.started);
  output = relay_case(NULL, 0, 0, 0);
  assert(output.result == -1 && output.error == ETIMEDOUT && !output.started && !output.length);

  char many[1024];
  size_t used = 0;
  const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
  for (size_t i = 0; i < 17; ++i) {
    memcpy(many + used, interim, sizeof(interim) - 1);
    used += sizeof(interim) - 1;
  }
  output = relay_case(many, used, 1, 0);
  assert(output.result == -1 && output.error == EPROTO && !output.started && !output.length);
  char oversized[8193];
  memset(oversized, 'a', sizeof(oversized));
  memcpy(oversized, "HTTP/1.1 200 OK\r\nX: ", 19);
  output = relay_case(oversized, sizeof(oversized), 1, 0);
  assert(output.result == -1 && output.error == EMSGSIZE && !output.started && !output.length);

  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  int started = 123;
  const volatile sig_atomic_t stopping = 1;
  assert(proxy_response_relay(sockets[0], sockets[1], 100, &stopping, -1, &started) == -1);
  assert(errno == ECANCELED && !started);
  assert(proxy_response_relay(sockets[0], sockets[1], 0, NULL, -1, &started) == -1 && errno == EINVAL);
  assert(proxy_response_relay(sockets[0], sockets[1], 100, NULL, -1, NULL) == -1 && errno == EINVAL);
  close(sockets[0]);
  close(sockets[1]);
  check_large(0);
  check_large(1);
  check_large(2);
  puts("Proxy response relay checks passed");
  return 0;
}
