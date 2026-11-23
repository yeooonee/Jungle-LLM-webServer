// HTTP 층: 요청 읽기 → 파싱(요청 라인·헤더·본문) → 응답 고르기
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "http.h"
#include "static.h"

int write_all(int fd, const char *buf, size_t len) {
    for (size_t sent = 0; sent < len; ) {
        ssize_t w = write(fd, buf + sent, len - sent);   // OS가 실제로 받아 간 바이트 수
        if (w < 0) return -1;
        sent += w;
    }
    return 0;
}

void send_head(int fd, int code, const char *reason, const char *type, size_t len) {
    char header[256];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, type, len);
    write_all(fd, header, n);
}

// 문자열이라 strlen으로 길이를 재도 안전
void send_response(int fd, int code, const char *reason, const char *body) {
    send_head(fd, code, reason, "text/plain", strlen(body));
    write_all(fd, body, strlen(body));
}

void handle_client(int client_fd, const char *root) {
    // 5-1. read: 헤더 끝(\r\n\r\n)이 보일 때까지 반복해서 읽기 (TCP는 나눠서 도착할 수 있음)
    // shortcut: 요청 전체가 buf(8KB)에 들어와야 함, 더 크면 413/400 → 큰 업로드가 필요할 때 동적 버퍼로
    char buf[8192];
    size_t total = 0;
    char *header_end = NULL;
    while (!header_end && total < sizeof(buf) - 1) {
        ssize_t n = read(client_fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;                                     // 0: 상대가 끊음, -1: 에러
        total += n;
        buf[total] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
    }
    if (total == 0) return;                                    // 아무것도 안 왔으면 응답 없이 close

    // 파싱 ① 요청 라인: "GET /index.html HTTP/1.1" → 공백 기준 3조각
    char method[16], path[1024], version[16];
    int ok = header_end && sscanf(buf, "%15s %1023s %15s", method, path, version) == 3;
    // 버전이 "HTTP/숫자.숫자" 형식이 아니면 요청 라인이 깨진 것 → 400 (RFC 9112)
    if (ok && !(strlen(version) == 8 && strncmp(version, "HTTP/", 5) == 0 &&
                isdigit((unsigned char)version[5]) && version[6] == '.' &&
                isdigit((unsigned char)version[7])))
        ok = 0;
    long content_length = 0;
    int has_te = 0;                                            // Transfer-Encoding 헤더가 있었는지
    int has_host = 0;                                          // Host 헤더가 있었는지

    // 파싱 ② 헤더: 첫 줄 다음부터 빈 줄(\r\n\r\n)까지 "이름: 값"
    if (ok) {
        printf("--- %s %s %s ---\n", method, path, version);
        char *line = strstr(buf, "\r\n");
        if (line) line += 2;                                   // 요청 라인 건너뛰기
        while (line && strncmp(line, "\r\n", 2) != 0) {        // 빈 줄 = 헤더 끝
            char *end = strstr(line, "\r\n");
            if (!end) break;                                   // 줄이 잘려서 옴 (read 1번 한계)
            char *colon = memchr(line, ':', end - line);
            if (colon) {
                char *value = colon + 1;
                while (*value == ' ') value++;                 // ":" 뒤 공백 건너뛰기
                printf("  [%.*s] = [%.*s]\n",
                       (int)(colon - line), line, (int)(end - value), value);
                if (colon - line == 14 && strncasecmp(line, "Content-Length", 14) == 0)
                    content_length = strtol(value, NULL, 10);
                // shortcut: chunked 미지원 → 501로 거절, 스트림 업로드가 필요해지면 [C]를 chunk 읽기로 교체
                if (colon - line == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
                    has_te = 1;
                if (colon - line == 4 && strncasecmp(line, "Host", 4) == 0)
                    has_host = 1;
            }
            line = end + 2;                                    // 다음 줄로
        }
        // HTTP/1.1은 Host 필수, 없으면 400 (RFC 9112 3.2). HTTP/1.0은 선택
        if (!has_host && strcmp(version, "HTTP/1.1") == 0) ok = 0;
    }

    // 파싱 ③ 본문: 문자로 자르지 않고 Content-Length 바이트만큼 모일 때까지 read 반복
    int too_big = 0;
    if (ok && !has_te) {                                       // TE가 있으면 본문 길이를 모르니 읽지 않음
        char *body = header_end + 4;                           // 빈 줄 바로 다음이 본문 시작
        size_t room = sizeof(buf) - 1 - (body - buf);          // 본문이 들어갈 남은 공간
        size_t have = total - (body - buf);                    // 헤더와 같이 이미 도착한 본문
        if (content_length < 0) ok = 0;
        else if ((size_t)content_length > room) too_big = 1;
        else {
            while (have < (size_t)content_length) {
                ssize_t n = read(client_fd, buf + total, content_length - have);
                if (n <= 0) break;
                total += n;
                have += n;
            }
            if (have < (size_t)content_length) ok = 0;         // 다 오기 전에 끊김
            else printf("  body(%ld bytes) = [%.*s]\n", content_length, (int)content_length, body);
        }
    }

    // 처리 + 5-2. write: 파싱 결과에 따라 응답 골라 보내기
    if (too_big)
        send_response(client_fd, 413, "Payload Too Large", "Payload Too Large");
    else if (has_te)
        send_response(client_fd, 501, "Not Implemented", "Not Implemented");
    else if (!ok)
        send_response(client_fd, 400, "Bad Request", "Bad Request");
    // 형식은 맞지만 지원 안 하는 버전 (예: HTTP/2.0, HTTP/9.9) → 505
    else if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0)
        send_response(client_fd, 505, "HTTP Version Not Supported", "HTTP Version Not Supported");
    // 어떤 경로에서도 구현 안 한 메서드 → 501 (RFC 9110)
    else if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)
        send_response(client_fd, 501, "Not Implemented", "Not Implemented");
    else if (strcmp(method, "GET") == 0)
        serve_file(client_fd, root, path);
    else   // POST: 정적 파일은 받을 곳이 없음 (동적 처리는 Day 4)
        send_response(client_fd, 405, "Method Not Allowed", "Method Not Allowed");
}
