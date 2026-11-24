#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "cgi/run.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGI_TIMEOUT_SECONDS 3
#define CGI_MAX_OUTPUT_BYTES (HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_BYTES)

static int spawn_cgi(pid_t *pid, const char *program, char *const argv[],
                      char *const env[], const int input[2], const int output[2]) {
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error != 0) {
    return error;
  }
  posix_spawnattr_t attributes;
  error = posix_spawnattr_init(&attributes);
  if (error != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return error;
  }

  sigset_t mask;
  sigset_t defaults;
  if (sigemptyset(&mask) == -1 || sigemptyset(&defaults) == -1 ||
      sigaddset(&defaults, SIGPIPE) == -1 || sigaddset(&defaults, SIGINT) == -1 ||
      sigaddset(&defaults, SIGTERM) == -1) {
    error = errno;
    goto cleanup;
  }
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__linux__) && defined(__GLIBC__)
#if !__GLIBC_PREREQ(2, 34)
#error "CGI fd isolation requires glibc 2.34 or later"
#endif
#else
#error "CGI fd isolation supports macOS and Linux with glibc 2.34 or later"
#endif
  error = posix_spawnattr_setsigmask(&attributes, &mask);
  if (error == 0) {
    error = posix_spawnattr_setsigdefault(&attributes, &defaults);
  }
  if (error == 0) {
    error = posix_spawnattr_setflags(&attributes, flags);
  }
  if (error == 0) {
    error = posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
  }
  if (error == 0) {
    error = posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
  }
  /* macOS의 기본 상속 차단에서도 진단용 stderr는 명시적으로 유지한다. */
  if (error == 0) {
    error = posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDERR_FILENO);
  }
  const int pipes[] = {input[0], input[1], output[0], output[1]};
  for (size_t i = 0; error == 0 && i < sizeof(pipes) / sizeof(pipes[0]); ++i) {
    error = posix_spawn_file_actions_addclose(&actions, pipes[i]);
  }
#if defined(__linux__) && defined(__GLIBC__)
  if (error == 0) {
    error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  }
#endif
  if (error == 0) {
    error = posix_spawn(pid, program, &actions, &attributes, argv, env);
  }
cleanup:
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  return error;
}

static int check_cancel(const volatile sig_atomic_t *stopping, int cancel_fd) {
  if (stopping != NULL && *stopping) {
    errno = ECANCELED;
    return -1;
  }
  if (cancel_fd >= 0) {
    struct pollfd cancel = {.fd = cancel_fd, .events = POLLIN};
    int ready = poll(&cancel, 1, 0);
    if (ready == -1 && errno != EINTR) {
      return -1;
    }
    if (ready > 0 && (cancel.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      errno = ECANCELED;
      return -1;
    }
  }
  return 0;
}

static int time_left_ms(const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
    perror("clock_gettime");
    return -1;
  }
  time_t seconds = deadline->tv_sec - now.tv_sec;
  long nanoseconds = deadline->tv_nsec - now.tv_nsec;
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
    fprintf(stderr, "CGI timeout\n");
    errno = ETIMEDOUT;
    return 0;
  }
  return (int)(seconds * 1000 + (nanoseconds + 999999L) / 1000000L);
}

static int relay_cgi(int input_fd, int output_fd, http_slice body,
                     const struct timespec *deadline, char *captured,
                     size_t *captured_length,
                     const volatile sig_atomic_t *stopping, int cancel_fd) {
  int result = -1;
  int saved_error;
  int flags = fcntl(input_fd, F_GETFL);
  if (flags == -1 || fcntl(input_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("nonblocking CGI input");
    goto cleanup;
  }

  char output[4096];
  size_t sent = 0;
  while (input_fd >= 0 || output_fd >= 0) {
    if (check_cancel(stopping, cancel_fd) == -1) {
      goto cleanup;
    }
    if (input_fd >= 0 && sent == body.length) {
      close(input_fd);
      input_fd = -1;
    }
    struct pollfd fds[] = {
        {.fd = input_fd, .events = POLLOUT},
        {.fd = output_fd, .events = POLLIN},
        {.fd = cancel_fd, .events = POLLIN},
    };
    if (input_fd < 0 && output_fd < 0) {
      break;
    }
    int timeout = time_left_ms(deadline);
    if (timeout <= 0) {
      goto cleanup;
    }
    if (poll(fds, 3, timeout) == -1) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll CGI");
      goto cleanup;
    }
    if (check_cancel(stopping, cancel_fd) == -1) {
      goto cleanup;
    }
    for (size_t i = 0; i < 2; ++i) {
      if (fds[i].revents & POLLNVAL) {
        fprintf(stderr, "Invalid CGI relay fd\n");
        errno = EBADF;
        goto cleanup;
      }
    }
    if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t count = read(output_fd, output, sizeof(output));
      if (count > 0) {
        if ((size_t)count > CGI_MAX_OUTPUT_BYTES - *captured_length) {
          fprintf(stderr, "CGI output too large\n");
          errno = EMSGSIZE;
          goto cleanup;
        }
        memcpy(captured + *captured_length, output, (size_t)count);
        *captured_length += (size_t)count;
      } else if (count == 0) {
        close(output_fd);
        output_fd = -1;
      } else if (errno != EINTR) {
        perror("read CGI output");
        goto cleanup;
      }
    }
    if (fds[0].revents & (POLLERR | POLLHUP)) {
      errno = EPIPE;
      perror("write CGI input");
      goto cleanup;
    }
    if (fds[0].revents & POLLOUT) {
      ssize_t count = write(input_fd, body.data + sent, body.length - sent);
      if (count > 0) {
        sent += (size_t)count;
      } else if (count == 0 ||
                 (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        if (count == 0) {
          errno = EIO;
        }
        perror("write CGI input");
        goto cleanup;
      }
    }
  }
  result = 0;
cleanup:
  saved_error = errno;
  if (input_fd >= 0) {
    close(input_fd);
  }
  if (output_fd >= 0) {
    close(output_fd);
  }
  errno = saved_error;
  return result;
}

int cgi_run(const char *program, http_slice body, cgi_output *output,
              const volatile sig_atomic_t *stopping, int cancel_fd) {
  if (program == NULL || *program == '\0' || output == NULL ||
      (body.data == NULL && body.length != 0) || body.length > HTTP_MAX_BODY_BYTES) {
    errno = EINVAL;
    return -1;
  }
  if (check_cancel(stopping, cancel_fd) == -1) {
    return -1;
  }
  for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
    if (fcntl(fd, F_GETFD) == -1) {
      perror("CGI standard fd");
      return -1;
    }
  }
  char content_length[64];
  int written = snprintf(content_length, sizeof(content_length),
                         "CONTENT_LENGTH=%zu", body.length);
  if (written < 0 || (size_t)written >= sizeof(content_length)) {
    fprintf(stderr, "Cannot format body length\n");
    errno = EIO;
    return -1;
  }
  char method[] = "REQUEST_METHOD=POST";
  char path[] = "PATH=/usr/bin:/bin";
  char *child_env[] = {method, content_length, path, NULL};
  char *child_argv[] = {(char *)program, NULL};
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1) {
    perror("clock_gettime");
    return -1;
  }
  deadline.tv_sec += CGI_TIMEOUT_SECONDS;
  int input_pipe[2];
  if (pipe(input_pipe) == -1) {
    perror("pipe");
    return -1;
  }

  int output_pipe[2];
  if (pipe(output_pipe) == -1) {
    perror("pipe");
    close(input_pipe[0]);
    close(input_pipe[1]);
    return -1;
  }

  pid_t child_pid;
  int error = spawn_cgi(&child_pid, program, child_argv, child_env,
                         input_pipe, output_pipe);
  close(input_pipe[0]);
  close(output_pipe[1]);
  if (error != 0) {
    close(input_pipe[1]);
    close(output_pipe[0]);
    fprintf(stderr, "posix_spawn: %s\n", strerror(error));
    errno = error;
    return -1;
  }

  char *captured = malloc(CGI_MAX_OUTPUT_BYTES);
  size_t captured_length = 0;
  int relay_result;
  if (captured == NULL) {
    perror("allocate CGI output");
    close(input_pipe[1]);
    close(output_pipe[0]);
    errno = ENOMEM;
    relay_result = -1;
  } else {
    relay_result = relay_cgi(input_pipe[1], output_pipe[0], body, &deadline,
                             captured, &captured_length, stopping, cancel_fd);
  }
  int status;
  while (relay_result == 0) {
    if (check_cancel(stopping, cancel_fd) == -1) {
      break;
    }
    int timeout = time_left_ms(&deadline);
    if (timeout <= 0) {
      break;
    }
    pid_t waited = waitpid(child_pid, &status, WNOHANG);
    if (waited == child_pid) {
      int result = -1;
      if (WIFEXITED(status)) {
        result = WEXITSTATUS(status);
        if (result == EXIT_SUCCESS) {
          cgi_response response;
          if (!cgi_response_parse(captured, captured_length, &response)) {
            fprintf(stderr, "Invalid CGI response\n");
            result = -1;
          } else {
            *output = (cgi_output){captured, captured_length, response};
            return 0;
          }
        }
      } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "CGI terminated by signal %d\n", WTERMSIG(status));
      }
      free(captured);
      errno = EPROTO;
      return result;
    }
    if (waited == -1) {
      if (errno == EINTR) {
        continue;
      }
      perror("waitpid");
      break;
    }
    /* waitpid에는 시간 지정이 없어 짧게 쉬면서 같은 기한을 확인한다. */
    if (poll(NULL, 0, timeout < 10 ? timeout : 10) == -1 && errno != EINTR) {
      perror("wait CGI");
      break;
    }
  }

  int saved_error = errno;
  /* 중계 실패·시간 초과·취소 후에도 자식을 반드시 종료·회수한다. */
  if (kill(child_pid, SIGKILL) == -1 && errno != ESRCH) {
    perror("kill CGI");
  }
  while (waitpid(child_pid, &status, 0) == -1) {
    if (errno == EINTR) {
      continue;
    }
    perror("waitpid");
    break;
  }
  free(captured);
  errno = saved_error;
  return -1;
}
