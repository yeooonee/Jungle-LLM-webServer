// 정적 파일 서빙: URL 경로 → 루트 안의 파일 → 응답
#ifndef STATIC_H
#define STATIC_H

#include <stddef.h>

void serve_file(int fd, const char *root, const char *url_path);

// 서버 시작 때 1번: mime.types 파일 읽어 확장자 → Content-Type 표 채우기. 실패 -1
int load_mime_types(const char *path);

// in → out으로 8KB씩 복사 (파일 → 소켓, Day 4엔 소켓 → 소켓). 실패 -1
int send_file_body(int out, int in);

// CGI(cgi.c)도 같은 규칙을 쓰려고 공개
// URL 디코딩 (%2e → '.'), '?'에서 멈춤. 깨진 %XX나 %00이면 -1
int url_decode(const char *src, char *dst, size_t size);
// errno → 404/403/500 응답
void send_fs_error(int fd);

#endif
