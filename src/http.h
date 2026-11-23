// HTTP 층: 요청 읽기·파싱, 응답 보내기
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

// 손님 한 명 처리: 요청 읽기 → 파싱 → 응답 (close는 호출한 쪽이 함)
void handle_client(int fd, const char *root);

// write는 덜 보낼 수 있음 → len 바이트를 다 보낼 때까지 반복. 실패(손님이 끊음 등) -1
int write_all(int fd, const char *buf, size_t len);

// 헤더만 보내기. 본문 길이는 호출하는 쪽이 정확히 알려줌 (바이너리도 OK)
void send_head(int fd, int code, const char *reason, const char *type, size_t len);

// 텍스트 응답 (에러 메시지용)
void send_response(int fd, int code, const char *reason, const char *body);

#endif
