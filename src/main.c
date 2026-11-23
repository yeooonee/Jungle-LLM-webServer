#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "net/tcp.h"

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number) {
  (void)signal_number;
  stopping = 1;
}

int main(int argc, char *argv[]) {
  long port = 8080;
  if (argc > 3) {
    fprintf(stderr, "Usage: %s [port: 1-65535] [static-root]\n", argv[0]);
    return EXIT_FAILURE;
  }
  if (argc >= 2) {
    char *end;
    errno = 0;
    port = strtol(argv[1], &end, 10);

    if (errno != 0 || end == argv[1] || *end != '\0' || port < 1 ||
        port > 65535) {
      fprintf(stderr, "Invalid port: %s\n", argv[1]);
      return EXIT_FAILURE;
    }
  }

  struct sigaction action = {0};
  sigemptyset(&action.sa_mask);
  action.sa_handler = request_stop;

  if (sigaction(SIGINT, &action, NULL) == -1 ||
      sigaction(SIGTERM, &action, NULL) == -1) {
    perror("sigaction");
    return EXIT_FAILURE;
  }

  action.sa_handler = SIG_IGN;

  if (sigaction(SIGPIPE, &action, NULL) == -1) {
    perror("sigaction SIGPIPE");
    return EXIT_FAILURE;
  }

  const char *root = argc == 3 ? argv[2] : "www";
  return tcp_http_server((unsigned short)port, root, &stopping) == 0
             ? EXIT_SUCCESS : EXIT_FAILURE;
}
