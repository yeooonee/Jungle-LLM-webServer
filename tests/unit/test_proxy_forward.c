#include "proxy/forward.h"
#include "net/dispatch.h"
#include "net/tcp.h"
#include "proxy/connect.h"
#include "proxy/deadline.h"
#include "proxy/relay.h"
#include "proxy/request.h"
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
#include <time.h>
#include <unistd.h>

struct backend_args {
  int listener;
  const char *response;
  long delay_ms;
  int cancel_write;
  char request[16384];
  size_t received;
};

static void pause_ms(long milliseconds) {
  struct timespec delay = {.tv_sec = milliseconds / 1000,
                           .tv_nsec = milliseconds % 1000 * 1000000};
  while (nanosleep(&delay, &delay) == -1) {
    assert(errno == EINTR);
  }
}

static ssize_t receive(int fd, char *buffer, size_t capacity) {
  struct pollfd pending = {.fd = fd, .events = POLLIN};
  assert(poll(&pending, 1, 3000) > 0);
  ssize_t count = recv(fd, buffer, capacity, 0);
  assert(count >= 0);
  return count;
}

static int listen_backend(unsigned short *port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd != -1);
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(fd, 1) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
  *port = ntohs(address.sin_port);
  return fd;
}

static void *backend_main(void *argument) {
  struct backend_args *args = argument;
  struct pollfd pending = {.fd = args->listener, .events = POLLIN};
  assert(poll(&pending, 1, 3000) > 0);
  int fd = accept(args->listener, NULL, NULL);
  assert(fd != -1);
  http_request request;
  for (;;) {
    assert(args->received < sizeof(args->request) - 1);
    ssize_t count = receive(fd, args->request + args->received,
                              sizeof(args->request) - 1 - args->received);
    assert(count > 0);
    args->received += (size_t)count;
    http_parse_result result = http_request_parse(args->request, args->received, &request);
    if (result == HTTP_PARSE_COMPLETE) {
      break;
    }
    assert(result == HTTP_PARSE_INCOMPLETE);
  }
  args->request[args->received] = '\0';
  if (args->cancel_write != -1) {
    assert(close(args->cancel_write) == 0);
  }
  pause_ms(args->delay_ms);
  if (args->response != NULL) {
    size_t length = strlen(args->response);
    size_t sent = 0;
    while (sent < length) {
      ssize_t count = send(fd, args->response + sent, length - sent, 0);
      if (count <= 0) {
        assert(count == -1 && (errno == EPIPE || errno == ECONNRESET));
        break;
      }
      sent += (size_t)count;
    }
    int result = shutdown(fd, SHUT_WR);
    assert(result == 0 || errno == ENOTCONN);
  }
  /* 호출자가 소유한 백엔드 연결을 성공·실패 모두 닫았는지 확인한다. */
  char extra;
  assert(receive(fd, &extra, 1) == 0);
  close(fd);
  return NULL;
}

struct output {
  int result;
  int error;
  char data[4096];
  size_t length;
};

static const char input[] = "POST /api/add?x=1 HTTP/1.1\r\nHost: public.example\r\n"
                            "Content-Length: 5\r\nConnection: X-Secret\r\n"
                            "X-Secret: hidden\r\nContent-Type: text/plain\r\n\r\na=1&b";

static struct output forward_case(const char *wire, long delay, int cancel, int refused, int through_server) {
  unsigned short port;
  int listener = listen_backend(&port);
  int cancellation[2] = {-1, -1};
  if (cancel) {
    assert(pipe(cancellation) == 0);
  }
  struct backend_args args = {.listener = listener, .response = wire,
                               .delay_ms = delay, .cancel_write = cancellation[1]};
  pthread_t backend;
  if (refused) {
    close(listener);
  } else {
    assert(pthread_create(&backend, NULL, backend_main, &args) == 0);
  }
  int client[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
  int flags = fcntl(client[0], F_GETFL);
  http_request request;
  assert(http_request_parse(input, sizeof(input) - 1, &request) == HTTP_PARSE_COMPLETE);
  proxy_backend endpoint = {"127.0.0.1", port};
  proxy_routes routes = {0};
  assert(proxy_routes_add(&routes, "/api", endpoint) == 0);
  struct output output = {0};
  const volatile sig_atomic_t stopping = 0;
  if (through_server) {
    assert(tcp_send_all(client[1], input, sizeof(input) - 1, &stopping) == 0);
    assert(shutdown(client[1], SHUT_WR) == 0);
    if (through_server == 2) {
      assert(dispatch_start(-1, NULL, &routes, &stopping) == 0);
      dispatch_client(client[0]);
      output.result = 0;
    } else {
      output.result = tcp_handle_client(client[0], -1, NULL, &routes, cancellation[0], &stopping);
    }
  } else {
    output.result = proxy_forward_request(client[0], &request, &endpoint, 100, NULL, cancellation[0]);
  }
  output.error = errno;
  if (through_server != 2) {
    assert(fcntl(client[0], F_GETFL) == flags);
    close(client[0]);
  }
  for (;;) {
    ssize_t count = receive(client[1], output.data + output.length,
                              sizeof(output.data) - 1 - output.length);
    if (count == 0) {
      break;
    }
    output.length += (size_t)count;
    assert(output.length < sizeof(output.data) - 1);
  }
  output.data[output.length] = '\0';
  close(client[1]);
  if (through_server == 2) {
    dispatch_stop();
  }
  if (!refused) {
    assert(pthread_join(backend, NULL) == 0);
    char expected_host[64];
    snprintf(expected_host, sizeof(expected_host), "Host: 127.0.0.1:%u\r\n", (unsigned)port);
    assert(strncmp(args.request, "POST /api/add?x=1 HTTP/1.1\r\n", 28) == 0);
    assert(strstr(args.request, expected_host) != NULL);
    assert(strstr(args.request, "Connection: close\r\n") != NULL);
    assert(strstr(args.request, "X-Secret") == NULL);
    assert(strstr(args.request, "\r\n\r\na=1&b") != NULL);
    close(listener);
  }
  if (cancel) {
    assert(fcntl(cancellation[0], F_GETFD) != -1);
    close(cancellation[0]);
  }
  return output;
}

static void check_shared_deadline(void) {
  unsigned short port;
  int listener = listen_backend(&port);
  struct backend_args args = {.listener = listener,
      .response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
      .delay_ms = 300, .cancel_write = -1};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, backend_main, &args) == 0);
  struct timespec deadline;
  assert(proxy_deadline_start(&deadline, 400) == 0);
  pause_ms(200);
  int fd = proxy_connect_until("127.0.0.1", port, &deadline, NULL, -1);
  assert(fd != -1);
  http_request parsed;
  assert(http_request_parse(input, sizeof(input) - 1, &parsed) == HTTP_PARSE_COMPLETE);
  proxy_request request;
  assert(proxy_request_build(&parsed, "127.0.0.1", port, &request) == 0);
  assert(proxy_request_send_until(fd, &request, &deadline, NULL, -1) == 0);
  free(request.headers);
  int client[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
  int started;
  assert(proxy_response_relay_until(fd, client[0], &deadline, NULL, -1, &started) == -1);
  assert(errno == ETIMEDOUT && !started);
  close(fd);
  close(client[0]);
  close(client[1]);
  assert(pthread_join(thread, NULL) == 0);
  close(listener);
}

static void check_request_error(const char *wire, int status, int stalled) {
  http_request request;
  assert(http_request_parse(wire, strlen(wire), &request) == HTTP_PARSE_COMPLETE);
  int client[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
  if (stalled) {
    int small = 1024;
    assert(setsockopt(client[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    int flags = fcntl(client[0], F_GETFL);
    assert(flags != -1 && fcntl(client[0], F_SETFL, flags | O_NONBLOCK) == 0);
    char fill[4096] = {0};
    while (send(client[0], fill, sizeof(fill), MSG_DONTWAIT) > 0) {
    }
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    assert(fcntl(client[0], F_SETFL, flags) == 0);
  }
  proxy_backend endpoint = {"127.0.0.1", 8001};
  int result = proxy_forward_request(client[0], &request, &endpoint, 50, NULL, -1);
  if (stalled) {
    assert(result == -1 && errno == ETIMEDOUT);
  } else {
    assert(result == 0);
    close(client[0]);
    client[0] = -1;
    char output[2048];
    size_t used = 0;
    ssize_t count;
    while ((count = receive(client[1], output + used, sizeof(output) - 1 - used)) > 0) {
      used += (size_t)count;
      assert(used < sizeof(output) - 1);
    }
    output[used] = '\0';
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", status);
    assert(strstr(output, prefix) == output);
    assert(strstr(output, "Connection: close\r\n") != NULL);
  }
  if (client[0] != -1) {
    assert(fcntl(client[0], F_GETFD) != -1);
    close(client[0]);
  }
  close(client[1]);
}

int main(void) {
  assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
  struct output output = forward_case("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 0, 0, 0, 0);
  assert(output.result == 0 && strstr(output.data, "\r\n\r\nhello") != NULL);
  output = forward_case("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "5\r\nhello\r\n0\r\n\r\n", 0, 0, 0, 0);
  assert(output.result == 0 && strstr(output.data, "5\r\nhello\r\n0\r\n\r\n") != NULL);
  output = forward_case("broken\r\n\r\n", 0, 0, 0, 0);
  assert(output.result == 0 && strstr(output.data, "HTTP/1.1 502 Bad Gateway\r\n") == output.data);
  output = forward_case(NULL, 0, 0, 1, 0);
  assert(output.result == 0 && strstr(output.data, "HTTP/1.1 502 Bad Gateway\r\n") == output.data);
  output = forward_case(NULL, 200, 0, 0, 0);
  assert(output.result == 0 && strstr(output.data, "HTTP/1.1 504 Gateway Timeout\r\n") == output.data);
  output = forward_case("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello", 0, 0, 0, 0);
  assert(output.result == -1 && output.error == EPROTO);
  assert(strstr(output.data, "HTTP/1.1 200 OK\r\n") == output.data && strstr(output.data, "502") == NULL);
  output = forward_case(NULL, 0, 1, 0, 0);
  assert(output.result == -1 && output.error == ECANCELED && output.length == 0);

  output = forward_case("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 0, 0, 0, 1);
  assert(output.result == 0 && strstr(output.data, "\r\n\r\nhello") != NULL);
  output = forward_case("broken\r\n\r\n", 0, 0, 0, 1);
  assert(output.result == 0 && strstr(output.data, "HTTP/1.1 502 Bad Gateway\r\n") == output.data);
  output = forward_case("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello", 0, 0, 0, 1);
  assert(output.result == -1 && output.error == EPROTO && strstr(output.data, "502") == NULL);
  output = forward_case("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 0, 0, 0, 2);
  assert(output.result == 0 && strstr(output.data, "\r\n\r\nhello") != NULL);
  output = forward_case(NULL, 0, 1, 0, 1);
  assert(output.result == -1 && output.error == ECANCELED && output.length == 0);

  int client[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
  http_request parsed;
  assert(http_request_parse(input, sizeof(input) - 1, &parsed) == HTTP_PARSE_COMPLETE);
  proxy_backend endpoint = {"127.0.0.1", 8001};
  const volatile sig_atomic_t stopping = 1;
  assert(proxy_forward_request(client[0], &parsed, &endpoint, 100, &stopping, -1) == -1);
  assert(errno == ECANCELED);
  parsed.version = (http_slice){"HTTP/1.0", 8};
  assert(proxy_forward_request(client[0], &parsed, &endpoint, 100, NULL, -1) == -1 && errno == EINVAL);
  assert(proxy_forward_request(client[0], NULL, &endpoint, 100, NULL, -1) == -1 && errno == EINVAL);
  char byte;
  assert(recv(client[1], &byte, 1, MSG_DONTWAIT) == -1);
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  close(client[0]);
  close(client[1]);
  check_request_error("PATCH /api HTTP/1.1\r\nHost: x\r\n\r\n", 501, 0);
  check_request_error("GET /api HTTP/1.1\r\nHost: x\r\nConnection: X Y\r\n\r\n", 400, 0);
  check_request_error("PATCH /api HTTP/1.1\r\nHost: x\r\n\r\n", 501, 1);
  char many_headers[2048] = "GET /api HTTP/1.1\r\nHost: x\r\n";
  for (size_t i = 0; i < 63; ++i) {
    strcat(many_headers, "X: a\r\n");
  }
  strcat(many_headers, "\r\n");
  check_request_error(many_headers, 431, 0);
  check_shared_deadline();
  puts("Proxy forwarding checks passed");
  return 0;
}
