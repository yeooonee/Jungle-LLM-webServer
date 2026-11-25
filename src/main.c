// 서버 뼈대: 실행 설정 → socket → bind → listen (1번) / accept → 작업 큐 (반복)
// 워커 스레드가 큐에서 꺼내 handle_client → close
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "http.h"
#include "cgi.h"
#include "pool.h"
#include "proxy.h"
#include "static.h"

#define BACKLOG 10   // listen 대기열 길이

// 실행 설정. 우선순위: 기본값 < 설정 파일 < CLI 인자
static struct {
    char port[16];
    char root[PATH_MAX];
    char mime_types[PATH_MAX];
    char cgi_root[PATH_MAX];
    char cgi_timeout[16];
    char cgi_max_output[16];
    char cgi_log[PATH_MAX];
    char workers[16];
    char queue_size[16];
    char read_timeout[16];
    char request_timeout[16];
    char backend_connect_timeout[16];
    char backend_read_timeout[16];
    char backend_max_response[16];
} conf = { "8080", "./www", "./mime.types", "./build/cgi-bin", "5", "1048576", "./cgi.log",
           "8", "16", "5", "10", "2", "5", "1048576" };

// 숫자 설정값 검사. strtol: end = 숫자 다음 글자 위치. "80abc"면 end가 'a'를 가리킴 → 거절
static long to_long(const char *s, long min, long max, const char *name) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) {
        fprintf(stderr, "bad %s: %s\n", name, s);
        exit(1);
    }
    return v;
}

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
        // route는 값이 여러 개 (접두사 + 백엔드들) → 키 뒤 나머지를 통째로 넘김
        if (!strcmp(key, "route")) {
            if (proxy_add_route(strstr(line, "route") + 5) < 0) { fprintf(stderr, "%s:%d: bad route\n", path, no); exit(1); }
            continue;
        }
        if (got == 2 && !strcmp(key, "port"))            snprintf(conf.port, sizeof(conf.port), "%s", value);
        else if (got == 2 && !strcmp(key, "root"))       snprintf(conf.root, sizeof(conf.root), "%s", value);
        else if (got == 2 && !strcmp(key, "mime_types")) snprintf(conf.mime_types, sizeof(conf.mime_types), "%s", value);
        else if (got == 2 && !strcmp(key, "cgi_root"))   snprintf(conf.cgi_root, sizeof(conf.cgi_root), "%s", value);
        else if (got == 2 && !strcmp(key, "cgi_timeout")) snprintf(conf.cgi_timeout, sizeof(conf.cgi_timeout), "%s", value);
        else if (got == 2 && !strcmp(key, "cgi_max_output")) snprintf(conf.cgi_max_output, sizeof(conf.cgi_max_output), "%s", value);
        else if (got == 2 && !strcmp(key, "cgi_log"))    snprintf(conf.cgi_log, sizeof(conf.cgi_log), "%s", value);
        else if (got == 2 && !strcmp(key, "workers"))    snprintf(conf.workers, sizeof(conf.workers), "%s", value);
        else if (got == 2 && !strcmp(key, "queue_size")) snprintf(conf.queue_size, sizeof(conf.queue_size), "%s", value);
        else if (got == 2 && !strcmp(key, "read_timeout")) snprintf(conf.read_timeout, sizeof(conf.read_timeout), "%s", value);
        else if (got == 2 && !strcmp(key, "request_timeout")) snprintf(conf.request_timeout, sizeof(conf.request_timeout), "%s", value);
        else if (got == 2 && !strcmp(key, "backend_connect_timeout")) snprintf(conf.backend_connect_timeout, sizeof(conf.backend_connect_timeout), "%s", value);
        else if (got == 2 && !strcmp(key, "backend_read_timeout")) snprintf(conf.backend_read_timeout, sizeof(conf.backend_read_timeout), "%s", value);
        else if (got == 2 && !strcmp(key, "backend_max_response")) snprintf(conf.backend_max_response, sizeof(conf.backend_max_response), "%s", value);
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

// 워커가 큐에서 꺼낸 연결 하나를 처리. root는 시작 때 1번 정하고 안 바뀜 → 여러 스레드가 읽기만 해서 안전
static char root[PATH_MAX];
static void serve_connection(int client_fd) {
    handle_client(client_fd, root);   // 5. read → 파싱 → write
    close(client_fd);                 // 6. close: 하위 소켓만 닫음
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

    long port = to_long(conf.port, 1, 65535, "port");
    long cgi_timeout = to_long(conf.cgi_timeout, 1, 3600, "cgi_timeout");
    long cgi_max = to_long(conf.cgi_max_output, 1, 64L * 1024 * 1024, "cgi_max_output");
    long workers = to_long(conf.workers, 1, 1024, "workers");
    long queue_size = to_long(conf.queue_size, 1, 65536, "queue_size");
    long read_timeout = to_long(conf.read_timeout, 1, 3600, "read_timeout");
    long request_timeout = to_long(conf.request_timeout, 1, 3600, "request_timeout");
    http_init((int)read_timeout, (int)request_timeout);
    proxy_init((int)to_long(conf.backend_connect_timeout, 1, 3600, "backend_connect_timeout"),
               (int)to_long(conf.backend_read_timeout, 1, 3600, "backend_read_timeout"),
               (size_t)to_long(conf.backend_max_response, 1, 64L * 1024 * 1024, "backend_max_response"));
    // 루트는 서버 도는 동안 안 바뀜 → 시작할 때 1번 진짜 경로로 (없으면 포트 열기 전에 종료)
    if (!realpath(conf.root, root)) { perror(conf.root); exit(1); }
    if (load_mime_types(conf.mime_types) < 0) { perror(conf.mime_types); exit(1); }

    // CGI: 프로그램 폴더 진짜 경로 + 로그 파일 (O_APPEND: 여러 번 써도 항상 끝에 덧붙임)
    static char cgi_root[PATH_MAX];
    if (!realpath(conf.cgi_root, cgi_root)) { perror(conf.cgi_root); exit(1); }
    int log_fd = open(conf.cgi_log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) { perror(conf.cgi_log); exit(1); }
    cgi_init(cgi_root, (int)cgi_timeout, (size_t)cgi_max, log_fd);

    // 끊긴 상대(손님 소켓, 먼저 끝난 CGI의 파이프)에 write하면 OS가 SIGPIPE를 보냄 → 기본 동작은 서버 종료
    // 무시하면 write가 -1(EPIPE)을 돌려주고, write_all이 그걸 실패로 처리
    signal(SIGPIPE, SIG_IGN);

    int server_fd = open_listen_socket((int)port);
    fcntl(server_fd, F_SETFD, FD_CLOEXEC);   // exec된 CGI 프로그램에 메인 소켓이 따라가지 않게
    if (pool_start((int)workers, (int)queue_size, serve_connection) < 0) { perror("pool_start"); exit(1); }
    printf("listening on http://localhost:%ld (%s, workers %ld, queue %ld)\n", port,
           proxy_enabled() ? "reverse proxy" : root, workers, queue_size);

    while (1) {
        // 4. accept: 대기열에서 한 명 꺼내 하위 소켓 생성 (없으면 블로킹)
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) { perror("accept"); continue; }
        // 다른 워커가 CGI를 fork하면 이 소켓도 자식에게 복사됨 → exec 때 닫히게.
        // 안 그러면 CGI가 끝날 때까지 손님이 연결 종료(EOF)를 못 받음
        // shortcut: accept~fcntl 사이에 다른 스레드가 fork하면 새어 나감 (틈은 μs 단위) → 문제 되면 fork를 rwlock으로 감싸기
        fcntl(client_fd, F_SETFD, FD_CLOEXEC);

        pool_push(client_fd);   // 큐가 꽉 차면 여기서 기다림 → 그동안 accept 안 함
    }
}
