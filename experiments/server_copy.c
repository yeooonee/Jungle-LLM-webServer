// Day 1 최소 TCP 서버: socket → bind → listen (1번) / accept → read/write → close (반복)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8070
#define BACKLOG 10   // listen 대기열 길이

int main(void) {
    // 1. socket: 메인 소켓 생성
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    // [SO_REUSEADDR] 3단계 '깨뜨려 보기'에서 이 두 줄을 주석 처리해 비교할 예정
    // int opt = 1;
    // setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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
            printf("--- request ---\n%s\n", buf);

            // 5-2. write: 고정 응답 보내기
            const char *res =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Hello, World!";
            write(client_fd, res, strlen(res));
        }

        // 6. close: 하위 소켓만 닫음 (메인 소켓은 계속 손님 받음)
        close(client_fd);
    }
}
