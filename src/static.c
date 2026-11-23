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

// 6단계: 확장자 → MIME 표. 서버 시작 때 mime.types 파일에서 1번 채움
// shortcut: 고정 배열 256칸, 넘는 줄은 무시 → 확장자가 더 많아지면 malloc으로
static struct { char ext[16]; char type[128]; } mimes[256];
static int mime_count = 0;

// 형식(Apache /etc/mime.types와 같음): "타입 확장자 확장자 ..." 한 줄씩, '#'은 주석
int load_mime_types(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *type = strtok(line, " \t\r\n");
        if (!type) continue;                                    // 빈 줄
        for (char *ext; (ext = strtok(NULL, " \t\r\n")) && mime_count < 256; mime_count++) {
            snprintf(mimes[mime_count].ext, sizeof(mimes[0].ext), "%s", ext);
            snprintf(mimes[mime_count].type, sizeof(mimes[0].type), "%s", type);
        }
    }
    fclose(f);
    return 0;
}

// 확장자로 표 찾기. 모르면 "그냥 바이트"
static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot)
        for (int i = 0; i < mime_count; i++)
            if (!strcasecmp(dot + 1, mimes[i].ext)) return mimes[i].type;
    return "application/octet-stream";
}

// 7단계: in에서 읽어 out으로 8KB씩 복사 (Day 4 프록시 중계에도 재사용). 실패하면 -1
int send_file_body(int out, int in) {
    char chunk[8192];
    ssize_t n;
    while ((n = read(in, chunk, sizeof(chunk))) > 0) {
        // write는 요청한 것보다 적게 보낼 수 있음 → 남은 만큼 다시
        for (ssize_t sent = 0; sent < n; ) {
            ssize_t w = write(out, chunk + sent, n - sent);
            if (w < 0) return -1;                               // 손님이 끊음
            sent += w;
        }
    }
    return n < 0 ? -1 : 0;
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
    send_file_body(fd, file);   // 헤더를 이미 보냈으니 실패해도 다른 응답은 못 보냄 → close만
    close(file);
}
