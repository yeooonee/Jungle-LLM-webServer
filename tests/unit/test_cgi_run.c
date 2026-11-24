#include "cgi/run.h"
#include "cgi/send.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  struct sigaction action = {0};
  action.sa_handler = SIG_IGN;
  assert(sigemptyset(&action.sa_mask) == 0);
  assert(sigaction(SIGPIPE, &action, NULL) == 0);

  cgi_output output = {.data = NULL, .length = 123};
  assert(cgi_run(NULL, (http_slice){NULL, 0}, &output, NULL, -1) == -1 && errno == EINVAL);
  assert(output.length == 123);
  assert(cgi_run("/bin/sh", (http_slice){NULL, 1}, &output, NULL, -1) == -1 && errno == EINVAL);
  assert(cgi_run("/bin/sh", (http_slice){"", HTTP_MAX_BODY_BYTES + 1}, &output, NULL, -1) == -1);
  assert(errno == EINVAL && output.length == 123);
  assert(cgi_run("/bin/sh", (http_slice){NULL, 0}, NULL, NULL, -1) == -1 && errno == EINVAL);
  const volatile sig_atomic_t cancelled = 1;
  assert(cgi_run("/bin/sh", (http_slice){NULL, 0}, &output, &cancelled, -1) == -1);
  assert(errno == ECANCELED && output.length == 123);
  int cancel_pipe[2];
  assert(pipe(cancel_pipe) == 0 && close(cancel_pipe[1]) == 0);
  assert(cgi_run("/bin/sh", (http_slice){NULL, 0}, &output, NULL, cancel_pipe[0]) == -1);
  assert(errno == ECANCELED && output.length == 123);
  assert(fcntl(cancel_pipe[0], F_GETFD) != -1);
  close(cancel_pipe[0]);

  const char failure[] = "exit 42\n";
  assert(cgi_run("/bin/sh", (http_slice){failure, sizeof(failure) - 1}, &output, NULL, -1) == 42);
  assert(output.data == NULL && output.length == 123);

  const char script[] = "printf 'Content-Type: text/plain\\r\\n\\r\\nhello'\n";
  assert(cgi_run("/bin/sh", (http_slice){script, sizeof(script) - 1}, &output, NULL, -1) == 0);
  assert(output.response.status == 200 && output.response.body.length == 5);
  assert(memcmp(output.response.body.data, "hello", 5) == 0);
  assert(output.response.body.data >= output.data);
  assert(output.response.body.data + 5 == output.data + output.length);

  int sockets[2];
  const volatile sig_atomic_t stopping = 0;
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(cgi_response_send(sockets[0], &output.response, &stopping) == 0);
  free(output.data);
  assert(shutdown(sockets[0], SHUT_WR) == 0);
  const char expected[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                          "Content-Length: 5\r\nConnection: close\r\n\r\nhello";
  char actual[256];
  size_t used = 0;
  ssize_t count;
  while ((count = recv(sockets[1], actual + used, sizeof(actual) - used, 0)) > 0) {
    used += (size_t)count;
    assert(used < sizeof(actual));
  }
  assert(count == 0 && used == sizeof(expected) - 1);
  assert(memcmp(actual, expected, used) == 0);
  close(sockets[0]);
  close(sockets[1]);
  puts("CGI execution to HTTP checks passed");
  return 0;
}
