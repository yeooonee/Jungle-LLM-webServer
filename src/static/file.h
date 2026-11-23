#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include "http/request.h"
#include <signal.h>

const char *static_file_mime(const char *path);
/* root_fd는 열린 루트 디렉터리다. 오류 응답 송신 성공도 0, I/O 실패는 -1이다. */
int static_file_send(int client_fd, int root_fd, http_slice target,
                     const volatile sig_atomic_t *stopping);

#endif
