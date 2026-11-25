#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <signal.h>
#include <stddef.h>

typedef struct {
  int status;
  const char *content_type;
  const void *body;
  size_t body_length;
} http_response;

/* 송신 없이 상태 코드·헤더·본문 포인터를 검증한다. 성공 0, 실패 -1과 errno. */
int http_response_validate(const http_response *response);

/* 송신 없이 헤더를 구성한다. 성공은 바이트 수, 실패는 -1과 errno다. */
int http_response_format_headers(const http_response *response, char *headers, size_t capacity);

/* 파일처럼 본문을 별도로 전송할 때 사용한다. */
int http_response_send_headers(int client_fd, const http_response *response,
                               const volatile sig_atomic_t *stopping);

/* SIGPIPE는 호출자가 무시해야 한다. 본문은 복사하지 않으며 소켓도 닫지 않는다. */
int http_response_send(int client_fd, const http_response *response,
                       const volatile sig_atomic_t *stopping);

#endif
