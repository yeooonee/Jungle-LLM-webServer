#ifndef PROXY_DEADLINE_H
#define PROXY_DEADLINE_H

#include <time.h>

int proxy_deadline_start(struct timespec *deadline, int timeout_ms);
/* 남은 밀리초를 올림한다. 만료는 0과 ETIMEDOUT, 시계 오류는 -1이다. */
int proxy_deadline_remaining(const struct timespec *deadline);

#endif
