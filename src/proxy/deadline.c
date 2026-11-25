#include "proxy/deadline.h"

#include <errno.h>

int proxy_deadline_start(struct timespec *deadline, int timeout_ms) {
  if (deadline == NULL || timeout_ms <= 0) {
    errno = EINVAL;
    return -1;
  }
  if (clock_gettime(CLOCK_MONOTONIC, deadline) == -1) {
    return -1;
  }
  deadline->tv_sec += timeout_ms / 1000;
  deadline->tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (deadline->tv_nsec >= 1000000000L) {
    ++deadline->tv_sec;
    deadline->tv_nsec -= 1000000000L;
  }
  return 0;
}

int proxy_deadline_remaining(const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
    return -1;
  }
  time_t seconds = deadline->tv_sec - now.tv_sec;
  long nanoseconds = deadline->tv_nsec - now.tv_nsec;
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
    errno = ETIMEDOUT;
    return 0;
  }
  return (int)(seconds * 1000 + (nanoseconds + 999999L) / 1000000L);
}
