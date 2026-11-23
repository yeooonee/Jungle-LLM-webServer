// 동적 처리 (CGI): /cgi-bin/이름 → 외부 프로그램 실행 → 출력을 응답으로
#ifndef CGI_H
#define CGI_H

#include <stddef.h>

// 서버 시작 때 1번: cgi_root는 realpath로 푼 진짜 경로, log_fd는 stderr를 덧붙일 파일
void cgi_init(const char *cgi_root, int timeout_sec, size_t max_output, int log_fd);

// 요청 1개 처리. body는 POST 본문(GET이면 NULL, 0), content_type은 없으면 ""
void serve_cgi(int fd, const char *method, const char *url_path,
               const char *body, size_t body_len, const char *content_type);

#endif
