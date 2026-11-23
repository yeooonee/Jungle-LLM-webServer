// 동적 처리 (CGI, RFC 3875): fork → (자식) dup2 + exec / (부모) 본문 쓰기 → 출력 모으기 → waitpid → 응답
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "cgi.h"
#include "http.h"
#include "static.h"

static char cgi_root[PATH_MAX];
static int cgi_timeout;        // 초
static size_t cgi_max_output;  // 바이트
static int cgi_log = 2;        // 자식의 stderr가 갈 곳

void cgi_init(const char *root, int timeout_sec, size_t max_output, int log_fd) {
    snprintf(cgi_root, sizeof(cgi_root), "%s", root);
    cgi_timeout = timeout_sec;
    cgi_max_output = max_output;
    cgi_log = log_fd;
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);   // 시계를 바꿔도 안 흔들리는 시간
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

// ① 자식: stdin/stdout/stderr를 바꿔 끼우고 프로그램으로 변신. 돌아오면 exec 실패
static void run_child(int client_fd, int in[2], int out[2], const char *prog, const char *method,
                      const char *query, size_t body_len, const char *content_type) {
    dup2(in[0], STDIN_FILENO);     // 프로그램의 scanf/read  ← 부모가 쓴 POST 본문
    dup2(out[1], STDOUT_FILENO);   // 프로그램의 printf      → 부모가 읽음
    dup2(cgi_log, STDERR_FILENO);  // 프로그램의 에러 출력   → 로그 파일
    close(in[0]); close(in[1]); close(out[0]); close(out[1]);
    close(client_fd);              // 손님 소켓은 부모만 다룸

    char len[32];
    snprintf(len, sizeof(len), "%zu", body_len);
    setenv("REQUEST_METHOD", method, 1);
    setenv("QUERY_STRING", query, 1);
    setenv("CONTENT_LENGTH", len, 1);
    setenv("CONTENT_TYPE", content_type, 1);

    // 로그 머리줄: 어느 요청이 남긴 에러인지 구분
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    dprintf(STDERR_FILENO, "[%s] %s %s?%s\n", ts, method, prog, query);

    execl(prog, prog, (char *)NULL);
    dprintf(STDERR_FILENO, "exec failed: %s\n", strerror(errno));
    _exit(127);                    // exit 대신 _exit: 부모에게서 복사된 stdio 버퍼를 또 비우지 않게
}

// ② 부모: 출력을 끝(EOF)까지 모으기. 0 성공 / 504 시간 초과 / 502 너무 큼·읽기 실패
static int collect_output(int out_fd, char *buf, size_t *len) {
    long deadline = now_ms() + cgi_timeout * 1000L;
    *len = 0;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return 504;
        struct pollfd p = { .fd = out_fd, .events = POLLIN };
        int r = poll(&p, 1, (int)left);           // 읽을 게 생기거나 남은 시간이 다 될 때까지 대기
        if (r < 0) { if (errno == EINTR) continue; return 502; }
        if (r == 0) return 504;
        ssize_t n = read(out_fd, buf + *len, cgi_max_output + 1 - *len);
        if (n < 0) { if (errno == EINTR) continue; return 502; }
        if (n == 0) return 0;                     // 자식 쪽 쓰는 끝이 전부 닫힘 = 출력 끝
        *len += n;
        if (*len > cgi_max_output) return 502;
    }
}

// ③ CGI 출력(헤더 + 빈 줄 + 본문) → HTTP 응답. 형식이 깨졌으면 -1
static int send_cgi_response(int fd, char *out, size_t len) {
    // 헤더 끝: 규격은 줄 끝을 \n으로 허용 → \r\n\r\n과 \n\n 중 먼저 나오는 쪽
    char *end = NULL, *body = NULL;
    for (size_t i = 0; i + 1 < len && !end; i++) {
        if (out[i] == '\n' && out[i + 1] == '\n') { end = out + i; body = out + i + 2; }
        else if (i + 3 < len && !memcmp(out + i, "\r\n\r\n", 4)) { end = out + i; body = out + i + 4; }
    }
    if (!end) return -1;
    *end = '\0';

    char status[64] = "";
    int has_type = 0, has_location = 0;
    // 넘겨줄 헤더 모으기 (\n → \r\n으로 다시 씀). 크기는 출력 길이 + 줄마다 \r 1개면 충분
    char *pass = malloc(len * 2 + 1);
    if (!pass) return -1;
    size_t pl = 0;
    for (char *line = strtok(out, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *colon = strchr(line, ':');
        if (!colon || colon == line) { free(pass); return -1; }   // "이름: 값" 형식이 아님
        char *value = colon + 1;
        while (*value == ' ') value++;
        size_t name_len = colon - line;
        if (name_len == 6 && !strncasecmp(line, "Status", 6)) {  // "Status: 404 Not Found" → 상태 줄로
            snprintf(status, sizeof(status), "%s", value);
            continue;                                             // 응답 헤더로는 안 보냄
        }
        if (name_len == 14 && !strncasecmp(line, "Content-Length", 14)) continue;  // 서버가 다시 계산
        if (name_len == 10 && !strncasecmp(line, "Connection", 10)) continue;      // 서버가 정함
        if (name_len == 12 && !strncasecmp(line, "Content-Type", 12)) has_type = 1;
        if (name_len == 8 && !strncasecmp(line, "Location", 8)) has_location = 1;
        pl += sprintf(pass + pl, "%s\r\n", line);
    }
    if (!has_type && !has_location) { free(pass); return -1; }   // 규격상 둘 중 하나는 필수
    if (!status[0]) snprintf(status, sizeof(status), "%s", has_location ? "302 Found" : "200 OK");
    // "404 Not Found"처럼 3자리 숫자로 시작해야 함
    if (strlen(status) < 3 || !isdigit((unsigned char)status[0]) || !isdigit((unsigned char)status[1]) ||
        !isdigit((unsigned char)status[2]) || (status[3] != ' ' && status[3] != '\0')) {
        free(pass);
        return -1;
    }

    size_t body_len = len - (body - out);
    char head[128];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\n", status);
    write_all(fd, head, n);
    write_all(fd, pass, pl);
    n = snprintf(head, sizeof(head), "Content-Length: %zu\r\nConnection: close\r\n\r\n", body_len);
    write_all(fd, head, n);
    write_all(fd, body, body_len);
    free(pass);
    return 0;
}

void serve_cgi(int fd, const char *method, const char *url_path,
               const char *body, size_t body_len, const char *content_type) {
    // 0. 경로 → 프로그램 파일. 정적 서빙과 같은 규칙 (디코딩 → realpath → 루트 안인지)
    char decoded[1024];
    if (url_decode(url_path, decoded, sizeof(decoded)) < 0) {
        send_response(fd, 400, "Bad Request", "Bad Request");
        return;
    }
    char full[PATH_MAX + 1024];
    snprintf(full, sizeof(full), "%s%s", cgi_root, decoded + strlen("/cgi-bin"));
    char prog[PATH_MAX];
    if (!realpath(full, prog)) { send_fs_error(fd); return; }
    size_t rlen = strlen(cgi_root);
    if (strncmp(prog, cgi_root, rlen) != 0 || prog[rlen] != '/') {   // cgi_root 자체(폴더)도 거절
        send_response(fd, 403, "Forbidden", "Forbidden");
        return;
    }
    struct stat st;
    if (stat(prog, &st) < 0) { send_fs_error(fd); return; }
    if (!S_ISREG(st.st_mode) || access(prog, X_OK) < 0) {          // 폴더거나 실행 권한 없음
        send_response(fd, 403, "Forbidden", "Forbidden");
        return;
    }
    const char *q = strchr(url_path, '?');
    const char *query = q ? q + 1 : "";   // 규격: QUERY_STRING은 디코딩하지 않은 원본

    char *out = malloc(cgi_max_output + 1);
    int in[2], outp[2];
    if (!out || pipe(in) < 0) { free(out); send_response(fd, 500, "Internal Server Error", "Internal Server Error"); return; }
    if (pipe(outp) < 0) {
        close(in[0]); close(in[1]); free(out);
        send_response(fd, 500, "Internal Server Error", "Internal Server Error");
        return;
    }

    // 1. fork: 0이면 자식, 양수면 부모(값 = 자식 pid)
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(outp[0]); close(outp[1]); free(out);
        send_response(fd, 500, "Internal Server Error", "Internal Server Error");
        return;
    }
    if (pid == 0) run_child(fd, in, outp, prog, method, query, body_len, content_type);

    // 2. 부모: 자식용 끝은 닫기. 특히 outp[1]을 안 닫으면 쓰는 끝이 남아 read가 EOF(0)를 영영 못 받음
    close(in[0]);
    close(outp[1]);
    // 본문 ≤ 8KB(http.c 버퍼) < 파이프 버퍼 → 자식이 안 읽어도 여기서 안 멈춤. 실패(자식이 먼저 끝남)는 무시
    if (body_len) write_all(in[1], body, body_len);
    close(in[1]);                                   // 자식에게 "입력 끝(EOF)"

    // 3. 출력 모으기 → 시간 초과·너무 크면 kill
    size_t len;
    int err = collect_output(outp[0], out, &len);
    close(outp[0]);
    if (err) kill(pid, SIGKILL);

    // 4. waitpid: 종료 코드 받기 = OS가 프로세스 표 칸을 지움 (안 하면 좀비)
    int wst;
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR) {}
    if (!err && !(WIFEXITED(wst) && WEXITSTATUS(wst) == 0)) err = 502;   // 시그널로 죽음·종료 코드 ≠ 0

    // 5. 응답
    if (!err && send_cgi_response(fd, out, len) < 0) err = 502;
    if (err == 504) send_response(fd, 504, "Gateway Timeout", "Gateway Timeout");
    else if (err)   send_response(fd, 502, "Bad Gateway", "Bad Gateway");
    free(out);
}
