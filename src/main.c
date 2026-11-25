#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/tcp.h"

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number) {
  (void)signal_number;
  stopping = 1;
}

static int parse_port(const char *text, unsigned short *port) {
  if (*text == '\0') {
    return 0;
  }
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9') {
      return 0;
    }
  }
  errno = 0;
  long value = strtol(text, NULL, 10);
  if (errno != 0 || value < 1 || value > 65535) {
    return 0;
  }
  *port = (unsigned short)value;
  return 1;
}

static int parse_backend(const char *ipv4, const char *port, proxy_backend *backend) {
  struct in_addr address;
  if (inet_pton(AF_INET, ipv4, &address) != 1) {
    fprintf(stderr, "Invalid proxy IPv4 address: %s\n", ipv4);
    return 0;
  }
  if (!parse_port(port, &backend->port)) {
    fprintf(stderr, "Invalid proxy port: %s\n", port);
    return 0;
  }
  backend->ipv4 = ipv4;
  return 1;
}

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s [port: 1-65535] [static-root] [cgi-program]"
                  " [--proxy IPv4 port:1-65535] [--proxy-path /prefix]\n"
                  "       or: %s [port] [static-root] [cgi-program]"
                  " --route /prefix IPv4 port [--route ...]\n", program, program);
}

int main(int argc, char *argv[]) {
  unsigned short port = 8080;
  const char *legacy_prefix = "/api";
  proxy_backend legacy_backend = {0};
  proxy_routes routes = {0};
  int proxy_set = 0;
  int path_set = 0;
  int positional_argc = 1;
  while (positional_argc < argc && strncmp(argv[positional_argc], "--", 2) != 0) {
    ++positional_argc;
  }
  for (int i = positional_argc; i < argc; ++i) {
    if (strcmp(argv[i], "--proxy") == 0 && !proxy_set && routes.count == 0 && argc - i >= 3) {
      if (!parse_backend(argv[i + 1], argv[i + 2], &legacy_backend)) {
        return EXIT_FAILURE;
      }
      proxy_set = 1;
      i += 2;
    } else if (strcmp(argv[i], "--proxy-path") == 0 && !path_set &&
               routes.count == 0 && argc - i >= 2) {
      if (!proxy_route_prefix_valid(argv[i + 1])) {
        fprintf(stderr, "Invalid proxy path: %s\n", argv[i + 1]);
        return EXIT_FAILURE;
      }
      legacy_prefix = argv[++i];
      path_set = 1;
    } else if (strcmp(argv[i], "--route") == 0 && !proxy_set && !path_set && argc - i >= 4) {
      const char *prefix = argv[i + 1];
      proxy_backend backend;
      if (!parse_backend(argv[i + 2], argv[i + 3], &backend)) {
        return EXIT_FAILURE;
      }
      if (proxy_routes_add(&routes, prefix, backend) == -1) {
        const char *reason = errno == EEXIST ? "Duplicate proxy backend" :
                             errno == ENOSPC ? "Too many proxy routes" :
                             errno == E2BIG ? "Too many backends for proxy path" : "Invalid proxy path";
        fprintf(stderr, "%s: %s\n", reason, prefix);
        return EXIT_FAILURE;
      }
      i += 3;
    } else {
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }
  if (path_set && !proxy_set) {
    fprintf(stderr, "--proxy-path requires --proxy\n");
    return EXIT_FAILURE;
  }
  if (proxy_set && proxy_routes_add(&routes, legacy_prefix, legacy_backend) == -1) {
    perror("proxy route");
    return EXIT_FAILURE;
  }
  if (positional_argc > 4) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  if (positional_argc >= 2 && !parse_port(argv[1], &port)) {
    fprintf(stderr, "Invalid port: %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  const char *root = positional_argc >= 3 ? argv[2] : "www";
  const char *cgi_program = positional_argc == 4 ? argv[3] : NULL;
  if (cgi_program != NULL && *cgi_program == '\0') {
    fprintf(stderr, "CGI program path must not be empty\n");
    return EXIT_FAILURE;
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

  return tcp_http_server(port, root, cgi_program, &routes, &stopping) == 0
             ? EXIT_SUCCESS : EXIT_FAILURE;
}
