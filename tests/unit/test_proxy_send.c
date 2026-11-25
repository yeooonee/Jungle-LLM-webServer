#include "proxy/connect.h"
#include "proxy/send.h"

#include <arpa/inet.h>
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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static proxy_request make_request(char **source, size_t body_length) {
  *source = malloc(body_length + 256);
  assert(*source != NULL);
  int length = snprintf(*source, 256,
                         "POST /api/echo HTTP/1.1\r\nHost: proxy\r\n"
                         "Content-Length: %zu\r\n\r\n", body_length);
  assert(length > 0 && length < 256);
  for (size_t i = 0; i < body_length; ++i) {
    (*source)[(size_t)length + i] = (char)(i % 256);
  }
  http_request parsed;
  assert(http_request_parse(*source, (size_t)length + body_length, &parsed) == HTTP_PARSE_COMPLETE);
  proxy_request request;
  assert(proxy_request_build(&parsed, "127.0.0.1", 8001, &request) == 0);
  return request;
}

static void read_exact(int fd, char *data, size_t length) {
  size_t received = 0;
  while (received < length) {
    struct pollfd pending = {.fd = fd, .events = POLLIN};
    assert(poll(&pending, 1, 3000) > 0);
    ssize_t count = recv(fd, data + received, length - received, 0);
    assert(count > 0);
    received += (size_t)count;
  }
}

static void make_pair(int sockets[2]) {
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  int flags = fcntl(sockets[0], F_GETFL);
  assert(flags != -1 && fcntl(sockets[0], F_SETFL, flags | O_NONBLOCK) == 0);
  int capacity = 1024;
  assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
}

struct reader_args {
  int fd;
  char *data;
  size_t length;
};

static void *delayed_reader(void *argument) {
  const struct reader_args *args = argument;
  struct timespec delay = {.tv_nsec = 100000000};
  assert(nanosleep(&delay, NULL) == 0);
  read_exact(args->fd, args->data, args->length);
  return NULL;
}

static void *cancel_sender(void *argument) {
  int fd = *(int *)argument;
  struct timespec delay = {.tv_nsec = 50000000};
  assert(nanosleep(&delay, NULL) == 0);
  assert(close(fd) == 0);
  return NULL;
}

static volatile sig_atomic_t interrupts;

static void interrupt_wait(int signal_number) {
  (void)signal_number;
  ++interrupts;
}

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = SIG_IGN;
  assert(sigemptyset(&action.sa_mask) == 0);
  assert(sigaction(SIGPIPE, &action, NULL) == 0);

  char *source;
  proxy_request request = make_request(&source, 7);
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener != -1);
  struct sockaddr_in address = {
      .sin_family = AF_INET, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
  assert(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
  socklen_t address_length = sizeof(address);
  assert(getsockname(listener, (struct sockaddr *)&address, &address_length) == 0);
  assert(listen(listener, 1) == 0);
  int backend = proxy_connect("127.0.0.1", ntohs(address.sin_port), 1000);
  assert(backend != -1);
  struct pollfd incoming = {.fd = listener, .events = POLLIN};
  assert(poll(&incoming, 1, 1000) == 1);
  int peer = accept(listener, NULL, NULL);
  assert(peer != -1);
  assert(proxy_request_send(backend, &request, 1000, NULL, -1) == 0);
  char small[512];
  size_t total = request.headers_length + request.body.length;
  assert(total < sizeof(small));
  read_exact(peer, small, total);
  assert(memcmp(small, request.headers, request.headers_length) == 0);
  assert(memcmp(small + request.headers_length, request.body.data, request.body.length) == 0);
  assert(send(peer, "ok", 2, 0) == 2);
  read_exact(backend, small, 2);
  assert(memcmp(small, "ok", 2) == 0);
  close(peer);
  close(backend);
  close(listener);
  free(request.headers);
  free(source);

  request = make_request(&source, HTTP_MAX_BODY_BYTES);
  total = request.headers_length + request.body.length;
  char *received = malloc(total);
  assert(received != NULL);
  int sockets[2];
  make_pair(sockets);
  struct reader_args reader = {sockets[1], received, total};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, delayed_reader, &reader) == 0);
  assert(proxy_request_send(sockets[0], &request, 2000, NULL, -1) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(memcmp(received, request.headers, request.headers_length) == 0);
  assert(memcmp(received + request.headers_length, request.body.data, request.body.length) == 0);
  close(sockets[0]);
  close(sockets[1]);
  free(received);

  make_pair(sockets);
  const volatile sig_atomic_t stopping = 1;
  assert(proxy_request_send(sockets[0], &request, 1000, &stopping, -1) == -1);
  assert(errno == ECANCELED);
  char byte;
  assert(recv(sockets[1], &byte, 1, MSG_DONTWAIT) == -1);
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  assert(proxy_request_send(sockets[0], NULL, 1000, NULL, -1) == -1 && errno == EINVAL);
  assert(proxy_request_send(sockets[0], &request, 0, NULL, -1) == -1 && errno == EINVAL);
  assert(proxy_request_send(sockets[1], &request, 1000, NULL, -1) == -1 && errno == EINVAL);

  int cancel_pipe[2];
  assert(pipe(cancel_pipe) == 0);
  assert(pthread_create(&thread, NULL, cancel_sender, &cancel_pipe[1]) == 0);
  assert(proxy_request_send(sockets[0], &request, 2000, NULL, cancel_pipe[0]) == -1);
  assert(errno == ECANCELED);
  assert(pthread_join(thread, NULL) == 0);
  assert(fcntl(cancel_pipe[0], F_GETFD) != -1);
  close(cancel_pipe[0]);
  close(sockets[0]);
  close(sockets[1]);

  make_pair(sockets);
  action.sa_handler = interrupt_wait;
  assert(sigaction(SIGALRM, &action, NULL) == 0);
  struct itimerval timer = {
      .it_interval = {.tv_usec = 10000}, .it_value = {.tv_usec = 10000}};
  assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
  struct timespec started;
  struct timespec finished;
  assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
  int result = proxy_request_send(sockets[0], &request, 100, NULL, -1);
  int error = errno;
  assert(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
  timer = (struct itimerval){0};
  assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
  double elapsed = (double)(finished.tv_sec - started.tv_sec) +
                   (finished.tv_nsec - started.tv_nsec) / 1000000000.0;
  assert(result == -1 && error == ETIMEDOUT && interrupts > 0);
  assert(elapsed >= 0.09 && elapsed < 1.0);
  int flags = fcntl(sockets[0], F_GETFL);
  assert(flags != -1 && (flags & O_NONBLOCK));
  close(sockets[1]);
  assert(proxy_request_send(sockets[0], &request, 1000, NULL, -1) == -1);
  assert(errno == EPIPE || errno == ECONNRESET);
  close(sockets[0]);
  free(request.headers);
  free(source);
  puts("Proxy request send checks passed");
  return 0;
}
