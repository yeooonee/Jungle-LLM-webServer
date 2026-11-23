#ifndef DYNAMIC_ADD_H
#define DYNAMIC_ADD_H

#include <stddef.h>
#include <signal.h>

#include "http/request.h"

/* a=<정수>&b=<정수>를 읽는다. 실패하면 출력값을 변경하지 않는다. */
int dynamic_add_parse(const char *body, size_t length, long *left, long *right);

/* 성공하면 1과 합계를, 결과 포인터가 없거나 범위를 넘으면 0을 반환한다. */
int dynamic_add(long left, long right, long *sum);

int dynamic_add_send(int client_fd, http_slice body,
                      const volatile sig_atomic_t *stopping);

#endif
