#include "cgi/run.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int probe(void) {
  sigset_t mask;
  assert(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
  const int signals[] = {SIGPIPE, SIGINT, SIGTERM};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
    struct sigaction action;
    assert(sigismember(&mask, signals[i]) == 0);
    assert(sigaction(signals[i], NULL, &action) == 0);
    assert(action.sa_handler == SIG_DFL);
  }
  for (int fd = 0; fd < 3; ++fd) {
    assert(fcntl(fd, F_GETFD) != -1);
  }
  /* 이 테스트가 여는 fd는 모두 256 미만이며 CLOEXEC을 설정하지 않는다. */
  for (int fd = 3; fd < 256; ++fd) {
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  }
  const char *length = getenv("CONTENT_LENGTH");
  assert(length != NULL && strcmp(getenv("REQUEST_METHOD"), "POST") == 0);
  size_t size = (size_t)strtoul(length, NULL, 10);
  char body[64];
  assert(size <= sizeof(body));
  assert(fread(body, 1, size, stdin) == size);
  assert(fgetc(stdin) == EOF);
  struct timespec delay = {.tv_nsec = 20000000};
  assert(nanosleep(&delay, NULL) == 0);
  assert(printf("Content-Type: application/octet-stream\r\n\r\n") > 0);
  assert(fwrite(body, 1, size, stdout) == size);
  assert(fflush(stdout) == 0);
  return 0;
}

struct worker_args {
  const char *program;
  int number;
};

static void *run_worker(void *argument) {
  const struct worker_args *args = argument;
  for (int i = 0; i < 4; ++i) {
    char body[32];
    int length = snprintf(body, sizeof(body), "worker=%d,round=%d", args->number, i);
    assert(length > 0 && (size_t)length < sizeof(body));
    cgi_output output;
    assert(cgi_run(args->program, (http_slice){body, (size_t)length}, &output, NULL, -1) == 0);
    assert(output.response.body.length == (size_t)length);
    assert(memcmp(output.response.body.data, body, (size_t)length) == 0);
    free(output.data);
  }
  sigset_t mask;
  assert(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
  assert(sigismember(&mask, SIGINT) == 1 && sigismember(&mask, SIGTERM) == 1);
  return NULL;
}

int main(int argc, char **argv) {
  (void)argc;
  const char *name = strrchr(argv[0], '/');
  if (name != NULL && strcmp(name + 1, "cgi-probe") == 0) {
    return probe();
  }
  char executable[PATH_MAX];
  assert(realpath(argv[0], executable) != NULL);
  char directory[] = "/tmp/jungle-cgi-inheritance-XXXXXX";
  assert(mkdtemp(directory) != NULL);
  char program[PATH_MAX];
  int length = snprintf(program, sizeof(program), "%s/cgi-probe", directory);
  assert(length > 0 && (size_t)length < sizeof(program));
  assert(symlink(executable, program) == 0);

  int sockets[2];
  int pipes[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(pipe(pipes) == 0);
  int folder = open(directory, O_RDONLY);
  assert(folder >= 3);
  const int parent_fds[] = {sockets[0], sockets[1], pipes[0], pipes[1], folder};
  for (size_t i = 0; i < sizeof(parent_fds) / sizeof(parent_fds[0]); ++i) {
    assert(parent_fds[i] < 256 && fcntl(parent_fds[i], F_GETFD) == 0);
  }
  struct sigaction ignored = {0};
  ignored.sa_handler = SIG_IGN;
  assert(sigemptyset(&ignored.sa_mask) == 0);
  assert(sigaction(SIGPIPE, &ignored, NULL) == 0);
  sigset_t blocked;
  sigset_t original;
  assert(sigemptyset(&blocked) == 0);
  assert(sigaddset(&blocked, SIGINT) == 0 && sigaddset(&blocked, SIGTERM) == 0);
  assert(pthread_sigmask(SIG_BLOCK, &blocked, &original) == 0);
  pthread_t threads[4];
  struct worker_args args[4];
  for (size_t i = 0; i < 4; ++i) {
    args[i] = (struct worker_args){program, (int)i};
    assert(pthread_create(&threads[i], NULL, run_worker, &args[i]) == 0);
  }
  for (size_t i = 0; i < 4; ++i) {
    assert(pthread_join(threads[i], NULL) == 0);
  }
  assert(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
  struct sigaction current;
  assert(sigaction(SIGPIPE, NULL, &current) == 0 && current.sa_handler == SIG_IGN);
  for (size_t i = 0; i < sizeof(parent_fds) / sizeof(parent_fds[0]); ++i) {
    assert(fcntl(parent_fds[i], F_GETFD) == 0);
    assert(close(parent_fds[i]) == 0);
  }
  assert(unlink(program) == 0 && rmdir(directory) == 0);
  puts("CGI fd and signal inheritance checks passed (4 threads, 16 executions)");
  return 0;
}
