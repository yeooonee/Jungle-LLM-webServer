// Day 1 최소 TCP 서버: socket → bind → listen (1번) / accept → read/write → close (반복)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BACKLOG 10   // listen 대기열 길이

int main(void) {
    // 1. socket: 메인 소켓 생성
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    // [SO_REUSEADDR] 3단계 '깨뜨려 보기'에서 이 두 줄을 주석 처리해 비교할 예정
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. bind: 메인 소켓에 IP/포트 연결
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 이 컴퓨터의 모든 IP
    addr.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }

    // 3. listen: 손님 받을 준비 + OS에 대기열 생성
    if (listen(server_fd, BACKLOG) < 0) { perror("listen"); exit(1); }
    printf("listening on http://localhost:%d\n", PORT);

    while (1) {
        // 4. accept: 대기열에서 한 명 꺼내 하위 소켓 생성 (없으면 블로킹)
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }

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
        if (total > 0) {                                           // 아무것도 안 왔으면 응답 없이 close

            // 파싱 ① 요청 라인: "GET /index.html HTTP/1.1" → 공백 기준 3조각
            char method[16], path[1024], version[16];
            int ok = header_end && sscanf(buf, "%15s %1023s %15s", method, path, version) == 3;
            long content_length = 0;
            int has_te = 0;                                        // Transfer-Encoding 헤더가 있었는지

            // 파싱 ② 헤더: 첫 줄 다음부터 빈 줄(\r\n\r\n)까지 "이름: 값"
            if (ok) {
                printf("--- %s %s %s ---\n", method, path, version);
                char *line = strstr(buf, "\r\n");
                if (line) line += 2;                               // 요청 라인 건너뛰기
                while (line && strncmp(line, "\r\n", 2) != 0) {    // 빈 줄 = 헤더 끝
                    char *end = strstr(line, "\r\n");
                    if (!end) break;                               // 줄이 잘려서 옴 (read 1번 한계)
                    char *colon = memchr(line, ':', end - line);
                    if (colon) {
                        char *value = colon + 1;
                        while (*value == ' ') value++;             // ":" 뒤 공백 건너뛰기
                        printf("  [%.*s] = [%.*s]\n",
                               (int)(colon - line), line, (int)(end - value), value);
                        if (colon - line == 14 && strncasecmp(line, "Content-Length", 14) == 0)
                            content_length = strtol(value, NULL, 10);
                        // shortcut: chunked 미지원 → 501로 거절, 스트림 업로드가 필요해지면 [C]를 chunk 읽기로 교체
                        if (colon - line == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0)
                            has_te = 1;
                    }
                    line = end + 2;                                // 다음 줄로
                }
            }

            // 파싱 ③ 본문: 문자로 자르지 않고 Content-Length 바이트만큼 모일 때까지 read 반복
            int too_big = 0;
            if (ok && !has_te) {                                   // TE가 있으면 본문 길이를 모르니 읽지 않음
                char *body = header_end + 4;                       // 빈 줄 바로 다음이 본문 시작
                size_t room = sizeof(buf) - 1 - (body - buf);      // 본문이 들어갈 남은 공간
                size_t have = total - (body - buf);                // 헤더와 같이 이미 도착한 본문
                if (content_length < 0) ok = 0;
                else if ((size_t)content_length > room) too_big = 1;
                else {
                    while (have < (size_t)content_length) {
                        ssize_t n = read(client_fd, buf + total, content_length - have);
                        if (n <= 0) break;
                        total += n;
                        have += n;
                    }
                    if (have < (size_t)content_length) ok = 0;     // 다 오기 전에 끊김
                    else printf("  body(%ld bytes) = [%.*s]\n", content_length, (int)content_length, body);
                }
            }

            // 처리: 파싱 결과(경로)에 따라 응답 고르기
            const char *res;
            if (too_big) {
                res = "HTTP/1.1 413 Payload Too Large\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 17\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "Payload Too Large";
            } else if (has_te) {
                res = "HTTP/1.1 501 Not Implemented\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 15\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "Not Implemented";
            } else if (!ok) {
                res = "HTTP/1.1 400 Bad Request\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 11\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "Bad Request";
            } else if (strcmp(path, "/") == 0) {
                res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 13\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "Hello, World!";
            } else {
                res = "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: 9\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "Not Found";
            }

            // 5-2. write: 고른 응답 보내기
            write(client_fd, res, strlen(res));
        }

        // 6. close: 하위 소켓만 닫음 (메인 소켓은 계속 손님 받음)
        close(client_fd);
    }
}
