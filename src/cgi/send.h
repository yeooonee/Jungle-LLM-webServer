#ifndef CGI_SEND_H
#define CGI_SEND_H

#include "cgi/response.h"

#include <signal.h>

/* 성공 0, 실패 -1과 errno. 소켓과 원본 CGI 버퍼의 소유권은 호출자에게 있다. */
int cgi_response_send(int client_fd, const cgi_response *response,
                       const volatile sig_atomic_t *stopping);

/* 설정된 CGI를 실행하고 결과를 송신한다. 실패는 500·502·504로 응답하며 취소 시 응답하지 않는다. */
int cgi_program_send(int client_fd, const char *program, http_slice body,
                      const volatile sig_atomic_t *stopping, int cancel_fd);

#endif
