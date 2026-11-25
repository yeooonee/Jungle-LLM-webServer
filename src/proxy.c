// Reverse Proxy: 손님 쪽에선 서버, 백엔드 쪽에선 클라이언트 (연결 2개를 잇는 구조)
// 흐름: route 고르기(가장 긴 접두사) → 백엔드 고르기(라운드 로빈) → connect → 요청 전달 → 응답 다 받기 → 손님에게
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "http.h"
#include "proxy.h"

#define MAX_ROUTES 16
#define MAX_BACKENDS 8

struct route {
    char prefix[256];
    int n;                                    // 백엔드 수
    struct sockaddr_in backends[MAX_BACKENDS];
};

// 시작 때 채우고 안 바뀜 → 워커들이 읽기만 해서 안전
static struct route routes[MAX_ROUTES];
static int route_count;
static int connect_timeout_ms, read_timeout_s;
static size_t max_response;
// 라운드 로빈 차례 (서버 전체 1개). 여러 워커가 동시에 +1 → atomic: 잠금 없이 '읽고 +1'을 한 번에
static atomic_uint rr;

int proxy_add_route(const char *spec) {
    if (route_count == MAX_ROUTES) return -1;
    struct route *r = &routes[route_count];
    char copy[4096], *save;
    snprintf(copy, sizeof(copy), "%s", spec);
    char *tok = strtok_r(copy, " \t\r\n", &save);
    if (!tok || tok[0] != '/' || strlen(tok) >= sizeof(r->prefix)) return -1;
    strcpy(r->prefix, tok);
    while ((tok = strtok_r(NULL, " \t\r\n", &save))) {
        char *colon = strrchr(tok, ':');
        if (!colon || r->n == MAX_BACKENDS) return -1;
        *colon = '\0';
        // 이름(localhost 등) → IP는 시작 때 1번만 풀어 둠 (요청마다 DNS 안 함)
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
        if (getaddrinfo(tok, colon + 1, &hints, &res) != 0) return -1;
        memcpy(&r->backends[r->n++], res->ai_addr, sizeof(struct sockaddr_in));
        freeaddrinfo(res);
    }
    if (r->n == 0) return -1;
    route_count++;
    return 0;
}

void proxy_init(int connect_timeout, int read_timeout, size_t max_resp) {
    connect_timeout_ms = connect_timeout * 1000;
    read_timeout_s = read_timeout;
    max_response = max_resp;
}

int proxy_enabled(void) { return route_count > 0; }

// 가장 긴 접두사. 접두사 뒤가 '/'·'?'·끝일 때만 일치 (/api가 /apix에 걸리지 않게)
static const struct route *find_route(const char *path) {
    const struct route *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < route_count; i++) {
        const char *p = routes[i].prefix;
        size_t len = strlen(p);
        if (strncmp(path, p, len) != 0) continue;
        char next = path[len];
        if (p[len - 1] != '/' && next && next != '/' && next != '?') continue;
        if (len > best_len) { best = &routes[i]; best_len = len; }
    }
    return best;
}

// connect에 기한 걸기: 비블로킹으로 시작 → poll로 '연결됨'을 기다림 → 다시 블로킹
// -1: 거절·에러 → 502 / -2: 시간 초과 → 504
static int connect_backend(const struct sockaddr_in *addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, (const struct sockaddr *)addr, sizeof(*addr));
    if (rc < 0 && errno == EINPROGRESS) {                  // 연결 진행 중 (3-way handshake)
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        rc = poll(&p, 1, connect_timeout_ms);
        if (rc == 0) { close(fd); return -2; }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);  // 연결 결과 (0 = 성공, ECONNREFUSED 등)
        rc = (rc < 0 || err) ? -1 : 0;
    }
    if (rc < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, flags);
    struct timeval tv = { .tv_sec = read_timeout_s };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

// out에 이어 쓰기. 넘치면 -1
static int append(char *out, size_t size, size_t *n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *n, size - *n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= size - *n) return -1;
    *n += w;
    return 0;
}

static int is_header(const char *line, size_t name_len, const char *name) {
    return name_len == strlen(name) && strncasecmp(line, name, name_len) == 0;
}

// 요청 다시 쓰기: 요청 라인(경로 바뀜) + 헤더(연결별 헤더 빼고 X-Forwarded-For 덧붙임) + Connection: close
static int build_request(char *out, size_t size, size_t *n, const char *method, const char *path,
                         const char *headers, size_t headers_len, const char *client_ip) {
    *n = 0;
    if (append(out, size, n, "%s %s HTTP/1.1\r\n", method, path) < 0) return -1;
    int xff_done = 0;
    for (const char *line = headers; line < headers + headers_len; ) {
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        const char *colon = memchr(line, ':', end - line);
        size_t name_len = colon ? (size_t)(colon - line) : 0;
        int len = (int)(end - line);
        // 연결별(hop-by-hop) 헤더는 손님↔프록시 연결 얘기라 백엔드에 안 넘김 (RFC 9110 7.6.1)
        // shortcut: Connection 값에 적힌 추가 헤더 이름까지는 안 지움
        if (is_header(line, name_len, "Connection") || is_header(line, name_len, "Keep-Alive") ||
            is_header(line, name_len, "Proxy-Connection")) {
            ;
        } else if (is_header(line, name_len, "X-Forwarded-For")) {   // 앞 프록시가 남긴 값 뒤에 이어 붙임
            if (append(out, size, n, "%.*s, %s\r\n", len, line, client_ip) < 0) return -1;
            xff_done = 1;
        } else if (append(out, size, n, "%.*s\r\n", len, line) < 0) {
            return -1;
        }
        line = end + 2;
    }
    if (!xff_done && append(out, size, n, "X-Forwarded-For: %s\r\n", client_ip) < 0) return -1;
    return append(out, size, n, "Connection: close\r\n\r\n");   // 백엔드가 응답 끝에 연결을 닫음 → EOF = 응답 끝
}

// 응답을 EOF까지 모음 (Connection: close라 백엔드가 닫으면 끝). 0: 성공, -1: 에러·상한 초과, -2: 시간 초과
static int read_response(int fd, char **out, size_t *out_len) {
    size_t cap = 16384, n = 0;
    char *buf = malloc(cap + 1);                           // +1: 끝에 '\0' (헤더를 strstr로 찾으려고)
    if (!buf) return -1;
    for (;;) {
        if (n == cap) {                                    // 꽉 참 → 2배로 (상한 + 1까지만)
            if (n > max_response) { free(buf); return -1; }
            cap = cap * 2 > max_response + 1 ? max_response + 1 : cap * 2;
            char *bigger = realloc(buf, cap + 1);
            if (!bigger) { free(buf); return -1; }
            buf = bigger;
        }
        // shortcut: 기한이 read 한 번마다라 조금씩 계속 보내는 백엔드는 끝없이 기다림 → 필요하면 전체 기한 추가
        ssize_t r = read(fd, buf + n, cap - n);
        if (r == 0) break;
        if (r < 0) { free(buf); return (errno == EAGAIN || errno == EWOULDBLOCK) ? -2 : -1; }
        n += r;
    }
    if (n > max_response) { free(buf); return -1; }
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

// 받은 응답이 온전한가: "HTTP/"로 시작 + 헤더 끝 있음 + Content-Length가 있으면 본문이 그만큼 옴
static int response_ok(const char *buf, size_t len) {
    if (len < 5 || memcmp(buf, "HTTP/", 5) != 0) return 0;
    const char *head_end = strstr(buf, "\r\n\r\n");
    if (!head_end) return 0;
    size_t body_len = len - (head_end + 4 - buf);
    for (const char *line = strstr(buf, "\r\n") + 2; line < head_end; line = strstr(line, "\r\n") + 2) {
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            return strtoul(line + 15, NULL, 10) <= body_len;   // 덜 왔으면 중간에 끊긴 것
    }
    return 1;
}

void serve_proxy(int fd, const char *method, const char *path,
                 const char *headers, size_t headers_len,
                 const char *body, size_t body_len) {
    const struct route *r = find_route(path);
    if (!r) { send_response(fd, 404, "Not Found", "Not Found"); return; }

    // 접두사 떼기: /api/users?x=1 → /users?x=1, /api → /
    // shortcut: 경로를 정규화하지 않음 (/api/../x → /../x 그대로) → 백엔드가 막는다고 가정
    const char *rest = path + strlen(r->prefix);
    char new_path[1100];
    snprintf(new_path, sizeof(new_path), "%s%s", rest[0] == '/' ? "" : "/", rest);

    // 백엔드는 프록시만 보이니 손님 IP를 헤더로 알려 줌
    char ip[INET_ADDRSTRLEN] = "unknown";
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0)
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

    char req[10240];                                       // 손님 헤더(8KB 이하) + 바뀐 줄들
    size_t req_len;
    if (build_request(req, sizeof(req), &req_len, method, new_path, headers, headers_len, ip) < 0) {
        send_response(fd, 502, "Bad Gateway", "Bad Gateway");
        return;
    }

    // 멱등 = 여러 번 보내도 결과가 같음 → 보낸 뒤 타임아웃이어도 다른 백엔드에 다시 보내도 안전
    int idempotent = !strcmp(method, "GET") || !strcmp(method, "HEAD") ||
                     !strcmp(method, "PUT") || !strcmp(method, "DELETE");
    unsigned start = atomic_fetch_add(&rr, 1);
    int code = 502;
    for (int i = 0; i < r->n; i++) {
        const struct sockaddr_in *b = &r->backends[(start + i) % r->n];
        char bip[INET_ADDRSTRLEN];                         // inet_ntoa는 static 버퍼라 스레드에 안전하지 않음 → inet_ntop
        inet_ntop(AF_INET, &b->sin_addr, bip, sizeof(bip));
        printf("  proxy %s %s → %s:%d%s\n", method, path, bip, ntohs(b->sin_port), new_path);
        int bfd = connect_backend(b);
        if (bfd < 0) { code = bfd == -2 ? 504 : 502; continue; }   // 아직 아무것도 안 보냄 → 다음 백엔드

        char *resp = NULL;
        size_t resp_len = 0;
        int rc = -1;
        if (write_all(bfd, req, req_len) == 0 && write_all(bfd, body, body_len) == 0)
            rc = read_response(bfd, &resp, &resp_len);
        close(bfd);
        if (rc == 0 && response_ok(resp, resp_len)) {
            write_all(fd, resp, resp_len);                 // 다 받은 걸 그대로 손님에게
            free(resp);
            return;
        }
        free(resp);
        if (rc == -2) {                                    // 보냈는데 응답이 기한 안에 안 옴
            code = 504;
            if (idempotent) continue;                      // GET 등은 다음 백엔드로, POST는 중복 실행 위험 → 504
        } else {
            code = 502;                                    // 연결 끊김·깨진 응답·상한 초과
        }
        break;
    }
    if (code == 504) send_response(fd, 504, "Gateway Timeout", "Gateway Timeout");
    else send_response(fd, 502, "Bad Gateway", "Bad Gateway");
}
