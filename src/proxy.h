// Reverse Proxy: 경로 접두사로 route → 라운드 로빈으로 백엔드 → 요청 전달 → 응답을 다 받아 손님에게
#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>

// 설정 파일의 route 줄 1개: "접두사 호스트:포트 [호스트:포트 ...]". 깨졌으면 -1
int proxy_add_route(const char *spec);

// 서버 시작 때 1번: 백엔드 connect 기한, read 한 번 기한(초), 응답 상한(바이트)
void proxy_init(int connect_timeout_s, int read_timeout_s, size_t max_response);

// route 줄이 하나라도 있으면 1 → 서버 전체가 프록시 모드
int proxy_enabled(void);

// 요청 1개 중계. headers = 요청 라인 다음 줄부터 마지막 헤더의 \r\n까지 (빈 줄 제외)
void serve_proxy(int fd, const char *method, const char *path,
                 const char *headers, size_t headers_len,
                 const char *body, size_t body_len);

#endif
