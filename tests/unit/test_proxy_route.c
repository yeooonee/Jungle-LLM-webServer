#include "proxy/route.h"
#include "net/dispatch.h"
#include "net/tcp.h"

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
#include <unistd.h>

static const volatile sig_atomic_t stopping = 0;

static void check_http(int root_fd, const proxy_route *route, const char *wire,
                         int status, const char *body) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(tcp_send_all(pair[1], wire, strlen(wire), &stopping) == 0);
  assert(shutdown(pair[1], SHUT_WR) == 0);
  proxy_routes routes = {0};
  if (route != NULL) {
    for (size_t i = 0; i < route->backend_count; ++i) {
      assert(proxy_routes_add(&routes, route->prefix, route->backends[i]) == 0);
    }
  }
  assert(tcp_handle_client(pair[0], root_fd, NULL, &routes, -1, &stopping) == 0);
  close(pair[0]);
  char output[4096];
  size_t used = 0;
  for (;;) {
    struct pollfd pending = {.fd = pair[1], .events = POLLIN};
    assert(poll(&pending, 1, 1000) > 0 && used < sizeof(output) - 1);
    ssize_t count = recv(pair[1], output + used, sizeof(output) - 1 - used, 0);
    assert(count >= 0);
    if (count == 0) {
      break;
    }
    used += (size_t)count;
  }
  output[used] = '\0';
  close(pair[1]);
  char expected[32];
  snprintf(expected, sizeof(expected), "HTTP/1.1 %d ", status);
  assert(strncmp(output, expected, strlen(expected)) == 0);
  if (body != NULL) {
    const char *actual = strstr(output, "\r\n\r\n");
    assert(actual != NULL && strcmp(actual + 4, body) == 0);
  }
}

static const proxy_route *select_path(proxy_routes *routes, const char *path) {
  return proxy_route_select(routes, (http_slice){path, strlen(path)});
}

static void check_multiple_routes(void) {
  const proxy_route rules[] = {
      {"/", {{"127.0.0.1", 8000}}, 1, 0},
      {"/api", {{"127.0.0.1", 8001}}, 1, 0},
      {"/api/admin", {{"127.0.0.1", 8002}}, 1, 0},
  };
  for (int reverse = 0; reverse <= 1; ++reverse) {
    proxy_routes routes = {0};
    assert(select_path(&routes, "/api") == NULL);
    for (size_t i = 0; i < 3; ++i) {
      assert(proxy_routes_add(&routes, rules[reverse ? 2 - i : i].prefix,
                                rules[reverse ? 2 - i : i].backends[0]) == 0);
    }
    assert(select_path(&routes, "/api/admin/users")->backends[0].port == 8002);
    assert(select_path(&routes, "/api/admin?x=1")->backends[0].port == 8002);
    assert(select_path(&routes, "/api/administrator")->backends[0].port == 8001);
    assert(select_path(&routes, "/api?next=/api/admin")->backends[0].port == 8001);
    assert(select_path(&routes, "/apix")->backends[0].port == 8000);
    assert(select_path(&routes, "/")->backends[0].port == 8000);
    assert(proxy_routes_add(&routes, "/api", (proxy_backend){"127.0.0.1", 8001}) == -1);
    assert(errno == EEXIST && routes.count == 3);
    assert(select_path(&routes, "/api")->backends[0].port == 8001);
  }
  proxy_routes routes = {0};
  char prefixes[PROXY_MAX_ROUTES][32];
  for (size_t i = 0; i < PROXY_MAX_ROUTES; ++i) {
    snprintf(prefixes[i], sizeof(prefixes[i]), "/route%zu", i);
    assert(proxy_routes_add(&routes, prefixes[i], (proxy_backend){"127.0.0.1", 8001}) == 0);
  }
  assert(routes.count == PROXY_MAX_ROUTES);
  assert(proxy_routes_add(&routes, "/overflow", (proxy_backend){"127.0.0.1", 8001}) == -1);
  assert(errno == ENOSPC && routes.count == PROXY_MAX_ROUTES);
  assert(select_path(&routes, "/route15")->backends[0].port == 8001);
  assert(select_path(&routes, "/missing") == NULL);
  assert(select_path(NULL, "/api") == NULL);
  routes = (proxy_routes){0};
  assert(proxy_routes_add(&routes, "/bad/", (proxy_backend){"127.0.0.1", 8001}) == -1);
  assert(errno == EINVAL && routes.count == 0);
  assert(proxy_routes_add(&routes, "/ok", (proxy_backend){"localhost", 8001}) == -1);
  assert(errno == EINVAL && routes.count == 0);
  assert(proxy_routes_add(&routes, "/ok", (proxy_backend){"127.0.0.1", 0}) == -1);
  assert(errno == EINVAL && routes.count == 0);
  assert(proxy_routes_add(NULL, rules[0].prefix, rules[0].backends[0]) == -1 && errno == EINVAL);
}

static void check_backend_pools(void) {
  proxy_routes routes = {0};
  for (size_t i = 0; i < PROXY_MAX_BACKENDS; ++i) {
    proxy_backend backend = {"127.0.0.1", (unsigned short)(8000 + i)};
    assert(proxy_routes_add(&routes, "/api", backend) == 0);
  }
  assert(routes.count == 1);
  const proxy_route *api = select_path(&routes, "/api/users");
  assert(api != NULL && api->backend_count == PROXY_MAX_BACKENDS);
  for (size_t i = 0; i < api->backend_count; ++i) {
    assert(api->backends[i].port == 8000 + i);
  }
  assert(proxy_routes_add(&routes, "/api", (proxy_backend){"127.0.0.1", 9000}) == -1);
  assert(errno == E2BIG && api->backend_count == PROXY_MAX_BACKENDS && routes.count == 1);
  assert(proxy_routes_add(&routes, "/api", (proxy_backend){"127.0.0.1", 8000}) == -1);
  assert(errno == EEXIST && api->backend_count == PROXY_MAX_BACKENDS);
  assert(proxy_routes_add(&routes, "/api", (proxy_backend){"localhost", 9000}) == -1);
  assert(errno == EINVAL && api->backend_count == PROXY_MAX_BACKENDS);
  assert(proxy_routes_add(&routes, "/other", (proxy_backend){"127.0.0.1", 8000}) == 0);
  assert(proxy_routes_add(&routes, "/other", (proxy_backend){"127.0.0.2", 8000}) == 0);
  assert(select_path(&routes, "/other")->backend_count == 2);
  char prefixes[PROXY_MAX_ROUTES][32];
  for (size_t i = routes.count; i < PROXY_MAX_ROUTES; ++i) {
    snprintf(prefixes[i], sizeof(prefixes[i]), "/rule%zu", i);
    assert(proxy_routes_add(&routes, prefixes[i], (proxy_backend){"127.0.0.1", 8000}) == 0);
  }
  assert(routes.count == PROXY_MAX_ROUTES);
  assert(proxy_routes_add(&routes, "/other", (proxy_backend){"127.0.0.1", 8001}) == 0);
  assert(select_path(&routes, "/other")->backend_count == 3);
  assert(proxy_routes_add(&routes, "/overflow", (proxy_backend){"127.0.0.1", 8000}) == -1);
  assert(errno == ENOSPC && routes.count == PROXY_MAX_ROUTES);
}

struct selection_args {
  proxy_route *route;
  size_t counts[PROXY_MAX_BACKENDS];
};

static void *select_concurrently(void *argument) {
  struct selection_args *args = argument;
  for (size_t i = 0; i < 12000; ++i) {
    const proxy_backend *backend = proxy_backend_next(args->route);
    assert(backend != NULL);
    size_t index = (size_t)(backend - args->route->backends);
    assert(index < args->route->backend_count);
    ++args->counts[index];
  }
  return NULL;
}

static void check_round_robin(void) {
  proxy_routes routes = {0};
  for (unsigned short i = 0; i < 3; ++i) {
    assert(proxy_routes_add(&routes, "/api", (proxy_backend){"127.0.0.1", (unsigned short)(8000 + i)}) == 0);
  }
  assert(proxy_routes_add(&routes, "/files", (proxy_backend){"127.0.0.1", 9000}) == 0);
  assert(proxy_routes_add(&routes, "/files", (proxy_backend){"127.0.0.1", 9001}) == 0);
  proxy_route *api = proxy_route_select(&routes, (http_slice){"/api", 4});
  proxy_route *files = proxy_route_select(&routes, (http_slice){"/files", 6});
  for (size_t i = 0; i < 9; ++i) {
    assert(proxy_backend_next(api)->port == 8000 + i % 3);
    assert(proxy_backend_next(files)->port == 9000 + i % 2);
  }
  assert(api->next_backend == 0 && files->next_backend == 1);
  assert(proxy_routes_add(&routes, "/single", (proxy_backend){"127.0.0.1", 7000}) == 0);
  proxy_route *single = proxy_route_select(&routes, (http_slice){"/single", 7});
  for (size_t i = 0; i < 4; ++i) {
    assert(proxy_backend_next(single)->port == 7000 && single->next_backend == 0);
  }
  proxy_route invalid = {0};
  assert(proxy_backend_next(NULL) == NULL && errno == EINVAL);
  assert(proxy_backend_next(&invalid) == NULL && errno == EINVAL);
  invalid.backend_count = PROXY_MAX_BACKENDS + 1;
  assert(proxy_backend_next(&invalid) == NULL && errno == EINVAL);

  pthread_t threads[8];
  struct selection_args args[8] = {0};
  for (size_t i = 0; i < 8; ++i) {
    args[i].route = api;
    assert(pthread_create(&threads[i], NULL, select_concurrently, &args[i]) == 0);
  }
  size_t totals[3] = {0};
  for (size_t i = 0; i < 8; ++i) {
    assert(pthread_join(threads[i], NULL) == 0);
    for (size_t j = 0; j < 3; ++j) {
      totals[j] += args[i].counts[j];
    }
  }
  assert(totals[0] == 32000 && totals[1] == 32000 && totals[2] == 32000);
  assert(api->next_backend == 0);
}

int main(void) {
  assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
  check_multiple_routes();
  check_backend_pools();
  check_round_robin();
  const char *matches[] = {"/api", "/api/", "/api/users", "/api?x=1", "/api/users?q=/other"};
  const char *misses[] = {"", "/", "/ap", "/apix", "/api.txt", "/API/users", "/?q=/api/", "/%61pi/"};
  for (size_t i = 0; i < sizeof(matches) / sizeof(matches[0]); ++i) {
    assert(proxy_route_matches("/api", (http_slice){matches[i], strlen(matches[i])}));
  }
  for (size_t i = 0; i < sizeof(misses) / sizeof(misses[0]); ++i) {
    assert(!proxy_route_matches("/api", (http_slice){misses[i], strlen(misses[i])}));
  }
  const char raw[] = {'/', 'a', 'p', 'i', '/', 'x'};
  assert(proxy_route_matches("/api", (http_slice){raw, sizeof(raw)}));
  assert(!proxy_route_matches("/api", (http_slice){raw, 3}));
  assert(!proxy_route_matches("/api", (http_slice){NULL, 0}));

  const char *valid_prefixes[] = {"/", "/api", "/service/v1", "/a-b_c", "/%61pi"};
  const char *invalid_prefixes[] = {"", "service", "/service/", "/a?b", "/a#b",
                                     "/a b", "/a\t", "/a\\b", "/한글"};
  for (size_t i = 0; i < sizeof(valid_prefixes) / sizeof(valid_prefixes[0]); ++i) {
    assert(proxy_route_prefix_valid(valid_prefixes[i]));
  }
  for (size_t i = 0; i < sizeof(invalid_prefixes) / sizeof(invalid_prefixes[0]); ++i) {
    assert(!proxy_route_prefix_valid(invalid_prefixes[i]));
  }
  assert(!proxy_route_prefix_valid(NULL));
  char long_prefix[HTTP_MAX_HEADER_BYTES + 2];
  memset(long_prefix, 'a', sizeof(long_prefix));
  long_prefix[0] = '/';
  long_prefix[HTTP_MAX_HEADER_BYTES] = '\0';
  assert(proxy_route_prefix_valid(long_prefix));
  long_prefix[HTTP_MAX_HEADER_BYTES] = 'a';
  long_prefix[HTTP_MAX_HEADER_BYTES + 1] = '\0';
  assert(!proxy_route_prefix_valid(long_prefix));
  assert(proxy_route_matches("/service/v1", (http_slice){"/service/v1?x=1", sizeof("/service/v1?x=1") - 1}));
  assert(!proxy_route_matches("/service/v1", (http_slice){"/service/v10", 12}));
  assert(proxy_route_matches("/", (http_slice){"/other?x=1", 10}));
  assert(!proxy_route_matches("/", (http_slice){"http://x/api", 12}));
  assert(!proxy_route_matches(NULL, (http_slice){"/api", 4}));
  assert(!proxy_route_matches("", (http_slice){"/api", 4}));

  char root[] = "/tmp/proxy-route-XXXXXX";
  assert(mkdtemp(root) != NULL);
  int root_fd = open(root, O_RDONLY | O_DIRECTORY);
  assert(root_fd != -1);
  int file_fd = openat(root_fd, "api", O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(file_fd != -1 && write(file_fd, "local\n", 6) == 6);
  close(file_fd);
  proxy_route route = {"/api", {{"127.0.0.1", 1}}, 1, 0};
  check_http(root_fd, NULL, "GET /api?x=1 HTTP/1.1\r\nHost: x\r\n\r\n", 200, "local\n");
  check_http(root_fd, &route, "GET /apix HTTP/1.1\r\nHost: x\r\n\r\n", 404, NULL);
  check_http(root_fd, &route, "GET /missing?q=/api/ HTTP/1.1\r\nHost: x\r\n\r\n", 404, NULL);
  check_http(root_fd, &route, "POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\na=1&b=2", 200, "{\"result\":3}");
  check_http(root_fd, &route, "GET /api HTTP/1.0\r\n\r\n", 505, NULL);
  check_http(root_fd, NULL, "GET /api HTTP/1.0\r\n\r\n", 200, "local\n");
  check_http(root_fd, &route, "PATCH /api HTTP/1.1\r\nHost: x\r\n\r\n", 501, NULL);
  check_http(root_fd, &route, "GET /api HTTP/1.1\r\nHost: x\r\n", 400, NULL);
  route.prefix = "/service";
  check_http(root_fd, &route, "GET /api HTTP/1.1\r\nHost: x\r\n\r\n", 200, "local\n");
  check_http(root_fd, &route, "GET /service?x=1 HTTP/1.0\r\n\r\n", 505, NULL);
  check_http(root_fd, &route, "GET /servicex HTTP/1.1\r\nHost: x\r\n\r\n", 404, NULL);
  route.prefix = "/";
  check_http(root_fd, &route, "GET /api HTTP/1.0\r\n\r\n", 505, NULL);
  assert(unlinkat(root_fd, "api", 0) == 0);
  close(root_fd);
  assert(rmdir(root) == 0);
  puts("Proxy route and HTTP dispatch checks passed");
  return 0;
}
