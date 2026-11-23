#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http/response.h"
#include "net/tcp.h"

const char *static_file_mime(const char *path) {
  const char *extension = strrchr(path, '.');
  static const struct { const char *extension; const char *type; } types[] = {
      {".html", "text/html; charset=utf-8"},
      {".htm", "text/html; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},
      {".js", "text/javascript; charset=utf-8"},
      {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
      {".gif", "image/gif"}, {".svg", "image/svg+xml"},
      {".txt", "text/plain; charset=utf-8"}, {".json", "application/json"},
  };
  if (extension != NULL) {
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
      if (strcasecmp(extension, types[i].extension) == 0) {
        return types[i].type;
      }
    }
  }
  return "application/octet-stream";
}

static int hex_digit(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int decode_path(http_slice target, char *path) {
  if (target.length == 0 || target.data[0] != '/') return 400;
  if (target.length > HTTP_MAX_HEADER_BYTES) return 400;
  size_t written = 0;
  for (size_t i = 0; i < target.length; ++i) {
    unsigned char c = (unsigned char)target.data[i];
    if (c == '?') break;
    if (c == '#') return 400;
    if (c == '%') {
      if (i + 2 >= target.length) return 400;
      int high = hex_digit((unsigned char)target.data[i + 1]);
      int low = hex_digit((unsigned char)target.data[i + 2]);
      if (high < 0 || low < 0) return 400;
      c = (unsigned char)(high * 16 + low);
      i += 2;
    }
    if (c < 0x20 || c == 0x7f) return 400;
    if (c == '\\') return 403;
    path[written++] = (char)c;
  }
  path[written] = '\0';
  char *segment = path;
  while (*segment != '\0') {
    segment += strspn(segment, "/");
    size_t length = strcspn(segment, "/");
    if (length == 2 && memcmp(segment, "..", 2) == 0) return 403;
    segment += length;
  }
  return 200;
}

static int file_error(int error) {
  switch (error) {
  case ENOENT: case ENOTDIR: case ENAMETOOLONG: return 404;
  case EACCES: case EPERM: case ELOOP: return 403;
  default: return 500;
  }
}

/* 각 디렉터리 fd를 기준으로 열어 경로 교체와 심볼릭 링크의 루트 탈출을 막는다. */
static int open_file(int root_fd, char *path, const char **name, int *status) {
  size_t path_length = strlen(path);
  int trailing_slash = path_length > 0 && path[path_length - 1] == '/';
  int directory = dup(root_fd);
  if (directory == -1) { *status = 500; return -1; }
  char *state;
  char *segment = strtok_r(path, "/", &state);
  *name = "index.html";
  while (segment != NULL) {
    int next = openat(directory, segment, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    int error = errno;
    close(directory);
    if (next == -1) { *status = file_error(error); return -1; }
    directory = next;
    *name = segment;
    segment = strtok_r(NULL, "/", &state);
  }
  struct stat info;
  if (fstat(directory, &info) == -1) {
    *status = 500;
    close(directory);
    return -1;
  }
  if (trailing_slash && !S_ISDIR(info.st_mode)) {
    *status = 404;
    close(directory);
    return -1;
  }
  if (S_ISDIR(info.st_mode)) {
    int index = openat(directory, "index.html", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    int error = errno;
    close(directory);
    if (index == -1) { *status = file_error(error); return -1; }
    directory = index;
    *name = "index.html";
  }
  return directory;
}

static int send_error(int client_fd, int status,
                       const volatile sig_atomic_t *stopping) {
  const char *body = "Internal Server Error\n";
  if (status == 400) body = "Bad Request\n";
  if (status == 403) body = "Forbidden\n";
  if (status == 404) body = "Not Found\n";
  http_response response = {status, "text/plain; charset=utf-8", body, strlen(body)};
  return http_response_send(client_fd, &response, stopping);
}

int static_file_send(int client_fd, int root_fd, http_slice target,
                     const volatile sig_atomic_t *stopping) {
  char path[HTTP_MAX_HEADER_BYTES + 1];
  int status = decode_path(target, path);
  if (status != 200) return send_error(client_fd, status, stopping);
  const char *name;
  int file_fd = open_file(root_fd, path, &name, &status);
  if (file_fd == -1) return send_error(client_fd, status, stopping);
  struct stat info;
  if (fstat(file_fd, &info) == -1 || info.st_size < 0 ||
      (uintmax_t)info.st_size > SIZE_MAX) {
    close(file_fd);
    return send_error(client_fd, 500, stopping);
  }
  if (!S_ISREG(info.st_mode)) {
    close(file_fd);
    return send_error(client_fd, 403, stopping);
  }
  http_response response = {200, static_file_mime(name), NULL, (size_t)info.st_size};
  int result = http_response_send_headers(client_fd, &response, stopping);
  size_t remaining = response.body_length;
  char buffer[16384];
  while (result == 0 && remaining > 0) {
    if (*stopping) { errno = ECANCELED; result = -1; break; }
    size_t length = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    ssize_t count = read(file_fd, buffer, length);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      if (count == 0) errno = EIO;
      result = -1;
      break;
    }
    result = tcp_send_all(client_fd, buffer, (size_t)count, stopping);
    remaining -= (size_t)count;
  }
  close(file_fd);
  return result;
}
