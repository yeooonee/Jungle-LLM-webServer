#include "proxy/route.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

static pthread_mutex_t selection_mutex = PTHREAD_MUTEX_INITIALIZER;

int proxy_route_prefix_valid(const char *prefix) {
  if (prefix == NULL || prefix[0] != '/') {
    return 0;
  }
  size_t length = strlen(prefix);
  if (length > HTTP_MAX_HEADER_BYTES || (length > 1 && prefix[length - 1] == '/')) {
    return 0;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)prefix[i];
    if (c <= 0x20 || c >= 0x7f || c == '?' || c == '#' || c == '\\') {
      return 0;
    }
  }
  return 1;
}

int proxy_route_matches(const char *prefix, http_slice target) {
  if (prefix == NULL || target.data == NULL || target.length == 0 || target.data[0] != '/') {
    return 0;
  }
  size_t length = strlen(prefix);
  if (length == 0 || target.length < length || memcmp(target.data, prefix, length) != 0) {
    return 0;
  }
  return length == 1 || target.length == length ||
         target.data[length] == '/' || target.data[length] == '?';
}

int proxy_routes_add(proxy_routes *routes, const char *prefix, proxy_backend backend) {
  struct in_addr address;
  if (routes == NULL || routes->count > PROXY_MAX_ROUTES ||
      !proxy_route_prefix_valid(prefix) || backend.ipv4 == NULL || backend.port == 0 ||
      inet_pton(AF_INET, backend.ipv4, &address) != 1) {
    errno = EINVAL;
    return -1;
  }
  for (size_t i = 0; i < routes->count; ++i) {
    proxy_route *route = &routes->items[i];
    if (strcmp(route->prefix, prefix) != 0) {
      continue;
    }
    for (size_t j = 0; j < route->backend_count; ++j) {
      if (route->backends[j].port == backend.port &&
          strcmp(route->backends[j].ipv4, backend.ipv4) == 0) {
        errno = EEXIST;
        return -1;
      }
    }
    if (route->backend_count == PROXY_MAX_BACKENDS) {
      errno = E2BIG;
      return -1;
    }
    route->backends[route->backend_count++] = backend;
    return 0;
  }
  if (routes->count == PROXY_MAX_ROUTES) {
    errno = ENOSPC;
    return -1;
  }
  routes->items[routes->count++] = (proxy_route){
      .prefix = prefix, .backends = {backend}, .backend_count = 1};
  return 0;
}

proxy_route *proxy_route_select(proxy_routes *routes, http_slice target) {
  if (routes == NULL || routes->count > PROXY_MAX_ROUTES) {
    return NULL;
  }
  proxy_route *selected = NULL;
  size_t longest = 0;
  for (size_t i = 0; i < routes->count; ++i) {
    proxy_route *route = &routes->items[i];
    if (proxy_route_matches(route->prefix, target)) {
      size_t length = strlen(route->prefix);
      if (length > longest) {
        selected = route;
        longest = length;
      }
    }
  }
  return selected;
}

const proxy_backend *proxy_backend_next(proxy_route *route) {
  if (route == NULL || route->backend_count == 0 || route->backend_count > PROXY_MAX_BACKENDS) {
    errno = EINVAL;
    return NULL;
  }
  int error = pthread_mutex_lock(&selection_mutex);
  if (error != 0) {
    errno = error;
    return NULL;
  }
  size_t index = route->next_backend;
  route->next_backend = (index + 1) % route->backend_count;
  error = pthread_mutex_unlock(&selection_mutex);
  if (error != 0) {
    errno = error;
    return NULL;
  }
  return &route->backends[index];
}
