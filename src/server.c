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

        // 5-1. read: 요청 메시지 읽기
        // shortcut: read 한 번만 호출, 요청이 버퍼보다 크거나 나눠 오면 잘림 → 요청 파싱 단계에서 보강
        char buf[4096];
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';

            // 파싱 ① 요청 라인: "GET /index.html HTTP/1.1" → 공백 기준 3조각
            char method[16], path[1024], version[16];
            int ok = sscanf(buf, "%15s %1023s %15s", method, path, version) == 3;

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
                    }
                    line = end + 2;                                // 다음 줄로
                }
            }

            // 처리: 파싱 결과(경로)에 따라 응답 고르기
            const char *res;
            if (!ok) {
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
