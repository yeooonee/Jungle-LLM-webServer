// 서버 뼈대: 실행 설정 → socket → bind → listen (1번) / accept → handle_client → close (반복)
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "http.h"
#include "static.h"

#define BACKLOG 10   // listen 대기열 길이

// 실행 설정. 우선순위: 기본값 < 설정 파일 < CLI 인자
static struct {
    char port[16];
    char root[PATH_MAX];
    char mime_types[PATH_MAX];
} conf = { "8080", "./www", "./mime.types" };

// "키 값" 줄을 읽어 conf에 채우기. 모르는 키나 깨진 줄이면 어느 줄인지 알리고 종료
static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[4160], key[32], value[4096];   // PATH_MAX는 OS마다 다름(macOS 1024, Linux 4096) → 고정 크기
    for (int no = 1; fgets(line, sizeof(line), f); no++) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        int got = sscanf(line, "%31s %4095s", key, value);
        if (got <= 0) continue;                                    // 빈 줄·주석
        if (got == 2 && !strcmp(key, "port"))            snprintf(conf.port, sizeof(conf.port), "%s", value);
        else if (got == 2 && !strcmp(key, "root"))       snprintf(conf.root, sizeof(conf.root), "%s", value);
        else if (got == 2 && !strcmp(key, "mime_types")) snprintf(conf.mime_types, sizeof(conf.mime_types), "%s", value);
        else { fprintf(stderr, "%s:%d: bad line\n", path, no); exit(1); }
    }
    fclose(f);
}

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
    // 0. 실행 설정: ./server [-c 설정파일] [-p 포트] [-r 루트]
    //    설정 파일을 먼저 읽고, 그다음 -p/-r로 덮어씀 (getopt: POSIX 표준 옵션 파서)
    const char *conf_path = "server.conf";
    const char *cli_port = NULL, *cli_root = NULL;
    for (int c; (c = getopt(argc, argv, "c:p:r:")) != -1; ) {
        if (c == 'c') conf_path = optarg;
        else if (c == 'p') cli_port = optarg;
        else if (c == 'r') cli_root = optarg;
        else { fprintf(stderr, "usage: %s [-c config] [-p port] [-r root]\n", argv[0]); exit(1); }
    }
    // 옛 방식(./server 8080 ./www)처럼 옵션 아닌 인자가 남으면 조용히 무시하지 말고 알림
    if (optind < argc) { fprintf(stderr, "usage: %s [-c config] [-p port] [-r root]\n", argv[0]); exit(1); }
    load_config(conf_path);
    if (cli_port) snprintf(conf.port, sizeof(conf.port), "%s", cli_port);
    if (cli_root) snprintf(conf.root, sizeof(conf.root), "%s", cli_root);

    // strtol: end = 숫자 다음 글자 위치. "80abc"면 end가 'a'를 가리킴 → 거절
    char *end;
    long port = strtol(conf.port, &end, 10);
    if (end == conf.port || *end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "bad port: %s\n", conf.port);
        exit(1);
    }
    // 루트는 서버 도는 동안 안 바뀜 → 시작할 때 1번 진짜 경로로 (없으면 포트 열기 전에 종료)
    static char root[PATH_MAX];
    if (!realpath(conf.root, root)) { perror(conf.root); exit(1); }
    if (load_mime_types(conf.mime_types) < 0) { perror(conf.mime_types); exit(1); }

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
