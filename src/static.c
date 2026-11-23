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
        if (write_all(out, chunk, n) < 0) return -1;            // 손님이 끊음
    }
    return n < 0 ? -1 : 0;
}

// 3-1단계: URL 디코딩 (%2e → '.'). 깨진 %XX나 %00이면 -1
int url_decode(const char *src, char *dst, size_t size) {
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
void send_fs_error(int fd) {
    if (errno == ENOENT || errno == ENOTDIR) send_response(fd, 404, "Not Found", "Not Found");
    else if (errno == EACCES)                send_response(fd, 403, "Forbidden", "Forbidden");
    else                                     send_response(fd, 500, "Internal Server Error", "Internal Server Error");
}

// 폴더인데 URL 끝에 '/'가 없을 때: 301로 '/' 붙인 주소 알려 주기 (Nginx·Apache 기본 동작)
// Location엔 인코딩된 원본(url_path)을 써야 %20 등이 안 깨짐. 쿼리는 '/' 뒤로 유지
static void send_redirect(int fd, const char *url_path) {
    // 경로 부분의 연속 '/'를 하나로 (//css/ 는 브라우저가 "css라는 호스트"로 해석 → 오픈 리다이렉트)
    char loc[1100];   // url_path는 http.c에서 1023자 제한
    size_t j = 0;
    for (const char *p = url_path; *p && *p != '?' && j < sizeof(loc) - 2; p++)
        if (!(*p == '/' && j > 0 && loc[j - 1] == '/')) loc[j++] = *p;
    loc[j++] = '/';
    loc[j] = '\0';
    char head[2300];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 301 Moved Permanently\r\n"
                     "Location: %s%s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     loc, url_path + strcspn(url_path, "?"));
    write_all(fd, head, n);
}

void serve_file(int fd, const char *root, const char *url_path) {
    // 3-1. 디코딩
    char decoded[1024];
    // '\'는 브라우저가 '/'처럼 취급 → 301 Location이 다른 호스트로 해석될 수 있어 거절
    if (url_decode(url_path, decoded, sizeof(decoded)) < 0 || decoded[0] != '/' || strchr(decoded, '\\')) {
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
    if (S_ISDIR(st.st_mode) && decoded[strlen(decoded) - 1] != '/') {
        send_redirect(fd, url_path);   // 폴더인데 끝에 '/' 없음 → 301
        close(file);
        return;
    }
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
