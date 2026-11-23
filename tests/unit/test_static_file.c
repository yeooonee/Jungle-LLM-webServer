#include "static/file.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  const struct { const char *path; const char *type; } cases[] = {
      {"index.html", "text/html; charset=utf-8"},
      {"index.HTM", "text/html; charset=utf-8"},
      {"main.css", "text/css; charset=utf-8"},
      {"main.js", "text/javascript; charset=utf-8"},
      {"picture.PNG", "image/png"},
      {"picture.jpg", "image/jpeg"},
      {"picture.jpeg", "image/jpeg"},
      {"picture.svg", "image/svg+xml"},
      {"file", "application/octet-stream"},
      {"file.unknown", "application/octet-stream"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    assert(strcmp(static_file_mime(cases[i].path), cases[i].type) == 0);
  }

  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  volatile sig_atomic_t stopping = 0;
  http_slice target = {"/index.html", sizeof("/index.html") - 1};
  /* 유효하지 않은 루트 fd로 내부 파일 열기 실패 경로를 확인한다. */
  assert(static_file_send(sockets[0], -1, target, &stopping) == 0);
  assert(shutdown(sockets[0], SHUT_WR) == 0);
  char buffer[1024];
  size_t used = 0;
  ssize_t count;
  while ((count = recv(sockets[1], buffer + used, sizeof(buffer) - 1 - used, 0)) > 0) {
    used += (size_t)count;
    assert(used < sizeof(buffer) - 1);
  }
  assert(count == 0);
  buffer[used] = '\0';
  assert(strstr(buffer, "HTTP/1.1 500 Internal Server Error\r\n") == buffer);
  assert(strstr(buffer, "Content-Length: 22\r\n") != NULL);
  assert(strstr(buffer, "\r\n\r\nInternal Server Error\n") != NULL);
  close(sockets[0]);
  close(sockets[1]);
  puts("Static file checks passed");
  return 0;
}
