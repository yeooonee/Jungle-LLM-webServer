// 서버 뼈대: 실행 설정 → socket → bind → listen (1번) / accept → handle_client → close (반복)
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "http.h"

#define BACKLOG 10   // listen 대기열 길이

// socket → bind → listen. 실패하면 서버를 못 여니 exit
static int open_listen_socket(int port) {
    // 1. socket: 메인 소켓 생성
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    // [SO_REUSEADDR] '깨뜨려 보기'에서 이 두 줄을 주석 처리해 비교
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. bind: 메인 소켓에 IP/포트 연결
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 이 컴퓨터의 모든 IP
    addr.sin_port = htons(port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }

    // 3. listen: 손님 받을 준비 + OS에 대기열 생성
    if (listen(server_fd, BACKLOG) < 0) { perror("listen"); exit(1); }
    return server_fd;
}

int main(int argc, char **argv) {
    // 0. 실행 설정: ./server <포트> <루트 폴더>
    if (argc != 3) { fprintf(stderr, "usage: %s <port> <root>\n", argv[0]); exit(1); }
    // strtol: end = 숫자 다음 글자 위치. "80abc"면 end가 'a'를 가리킴 → 거절
    char *end;
    long port = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "bad port: %s\n", argv[1]);
        exit(1);
    }
    // 루트는 서버 도는 동안 안 바뀜 → 시작할 때 1번 진짜 경로로 (없으면 포트 열기 전에 종료)
    static char root[PATH_MAX];
    if (!realpath(argv[2], root)) { perror(argv[2]); exit(1); }

    int server_fd = open_listen_socket((int)port);
    printf("listening on http://localhost:%ld (root: %s)\n", port, root);

    while (1) {
        // 4. accept: 대기열에서 한 명 꺼내 하위 소켓 생성 (없으면 블로킹)
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }

        handle_client(client_fd, root);   // 5. read → 파싱 → write

        // 6. close: 하위 소켓만 닫음 (메인 소켓은 계속 손님 받음)
        close(client_fd);
    }
}
