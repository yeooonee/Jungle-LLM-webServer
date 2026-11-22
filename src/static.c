// 정적 파일 서빙 2~7단계 (1단계 루트 설정은 main.c)
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "http.h"
#include "static.h"

// 6단계: 확장자 → MIME. 모르면 "그냥 바이트"
static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".js"))   return "text/javascript";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".gif"))  return "image/gif";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

// 3-1단계: URL 디코딩 (%2e → '.'). 깨진 %XX나 %00이면 -1
static int url_decode(const char *src, char *dst, size_t size) {
    size_t i = 0;
    for (; *src && *src != '?' && i < size - 1; src++) {   // '?' 뒤 쿼리는 파일 경로가 아님
        if (*src == '%') {
            if (!isxdigit((unsigned char)src[1]) || !isxdigit((unsigned char)src[2])) return -1;
            char hex[3] = { src[1], src[2], 0 };
            char c = (char)strtol(hex, NULL, 16);
            if (c == '\0') return -1;                           // 문자열을 중간에 끊는 공격
            dst[i++] = c;
            src += 2;
        } else dst[i++] = *src;
    }
    dst[i] = '\0';
    return 0;
}

// errno(OS가 알려준 실패 이유) → 상태 코드
static void send_fs_error(int fd) {
    if (errno == ENOENT || errno == ENOTDIR) send_response(fd, 404, "Not Found", "Not Found");
    else if (errno == EACCES)                send_response(fd, 403, "Forbidden", "Forbidden");
    else                                     send_response(fd, 500, "Internal Server Error", "Internal Server Error");
}

void serve_file(int fd, const char *root, const char *url_path) {
    // 3-1. 디코딩
    char decoded[1024];
    if (url_decode(url_path, decoded, sizeof(decoded)) < 0 || decoded[0] != '/') {
        send_response(fd, 400, "Bad Request", "Bad Request");
        return;
    }
    // 2. 루트 + URL 경로, '/'로 끝나면 index.html
    char full[PATH_MAX + 1024 + 16];
    snprintf(full, sizeof(full), "%s%s%s", root, decoded,
             decoded[strlen(decoded) - 1] == '/' ? "index.html" : "");

    // 3-2. 정규화: .., ., 심볼릭 링크까지 풀어 진짜 경로로
    char real[PATH_MAX];
    if (!realpath(full, real)) { send_fs_error(fd); return; }

    // 3-3. 검사: 진짜 경로가 루트 안에 있는가 ("/www" vs "/www2" 구분 위해 다음 글자도 확인)
    size_t rlen = strlen(root);
    if (strncmp(real, root, rlen) != 0 || (real[rlen] != '/' && real[rlen] != '\0')) {
        send_response(fd, 403, "Forbidden", "Forbidden");
        return;
    }

    // 4. 열기
    int file = open(real, O_RDONLY);
    if (file < 0) { send_fs_error(fd); return; }

    // 5. 크기 (Content-Length용). 디렉터리는 파일이 아니니 403
    struct stat st;
    if (fstat(file, &st) < 0) { send_fs_error(fd); close(file); return; }
    if (!S_ISREG(st.st_mode)) {
        send_response(fd, 403, "Forbidden", "Forbidden");
        close(file);
        return;
    }

    // 6~7. 헤더 → 파일 내용을 조각(8KB)씩 그대로 복사
    send_head(fd, 200, "OK", mime_type(real), (size_t)st.st_size);
    char chunk[8192];
    ssize_t n;
    while ((n = read(file, chunk, sizeof(chunk))) > 0)
        if (write(fd, chunk, n) < 0) break;                     // 손님이 끊음
    close(file);
}
