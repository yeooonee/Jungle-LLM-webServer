#ifndef CGI_RUN_H
#define CGI_RUN_H

#include "cgi/response.h"

#include <signal.h>

typedef struct {
  char *data;
  size_t length;
  cgi_response response;
} cgi_output;

/* 성공 0: 호출자가 output->data를 free한다. 실패 시 output은 변경하지 않는다.
 * 자식의 비정상 종료 코드는 양수, 실행·수집·파싱 오류는 -1이다.
 * 호출자가 SIGPIPE를 무시해야 하며 fd 0·1·2가 열려 있어야 한다.
 * 자식은 stdin·stdout·stderr만 상속하며, 시그널 마스크는 비우고
 * SIGPIPE·SIGINT·SIGTERM은 기본 처리로 실행한다. 경로는 서버 설정에서 정한다. */
/* cancel_fd는 취소용 파이프 읽기 끝이며 쓰기 끝을 닫아 취소를 알린다.
 * stopping=NULL, cancel_fd=-1이면 취소 감시를 사용하지 않는다.
 * 취소는 ECANCELED, 시간 초과는 ETIMEDOUT, 잘못된 출력은 EPROTO/EMSGSIZE이다. */
int cgi_run(const char *program, http_slice body, cgi_output *output,
              const volatile sig_atomic_t *stopping, int cancel_fd);

#endif
