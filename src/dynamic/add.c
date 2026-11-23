#include "add.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "http/response.h"

static int parse_integer(const char *data, size_t length, long *number) {
  if (length == 0) {
    return 0;
  }
  int negative = data[0] == '-';
  size_t start = negative ? 1 : 0;
  if (start == length) {
    return 0;
  }

  /* LONG_MIN의 절댓값은 long으로 표현할 수 없으므로 unsigned long에 누적한다. */
  unsigned long limit = negative ? 0UL - (unsigned long)LONG_MIN
                                 : (unsigned long)LONG_MAX;
  unsigned long value = 0;
  for (size_t i = start; i < length; ++i) {
    unsigned char c = (unsigned char)data[i];
    if (c < '0' || c > '9') {
      return 0;
    }
    unsigned long digit = c - '0';
    if (value > (limit - digit) / 10) {
      return 0;
    }
    value = value * 10 + digit;
  }
  if (negative && value == 0UL - (unsigned long)LONG_MIN) {
    *number = LONG_MIN;
  } else {
    *number = negative ? -(long)value : (long)value;
  }
  return 1;
}

int dynamic_add_parse(const char *body, size_t length, long *left, long *right) {
  if (body == NULL || left == NULL || right == NULL || length < 4 ||
      memcmp(body, "a=", 2) != 0) {
    return 0;
  }
  const char *separator = memchr(body + 2, '&', length - 2);
  if (separator == NULL) {
    return 0;
  }
  size_t left_length = (size_t)(separator - (body + 2));
  size_t remaining = length - (size_t)(separator + 1 - body);
  if (remaining < 2 || memcmp(separator + 1, "b=", 2) != 0) {
    return 0;
  }
  long parsed_left;
  long parsed_right;
  if (!parse_integer(body + 2, left_length, &parsed_left) ||
      !parse_integer(separator + 3, remaining - 2, &parsed_right)) {
    return 0;
  }
  *left = parsed_left;
  *right = parsed_right;
  return 1;
}

int dynamic_add(long left, long right, long *sum) {
  if (sum == NULL) {
    return 0;
  }
  /* 부호 있는 정수 오버플로는 계산 후가 아니라 계산 전에 검사해야 한다. */
  if ((right > 0 && left > LONG_MAX - right) ||
      (right < 0 && left < LONG_MIN - right)) {
    return 0;
  }
  *sum = left + right;
  return 1;
}

int dynamic_add_send(int client_fd, http_slice body,
                      const volatile sig_atomic_t *stopping) {
  long left;
  long right;
  long sum;
  char result_body[128];
  int status = 400;
  const char *response_body = "{\"error\":\"invalid input or sum out of range\"}";

  if (dynamic_add_parse(body.data, body.length, &left, &right) &&
      dynamic_add(left, right, &sum)) {
    int length = snprintf(result_body, sizeof(result_body), "{\"result\":%ld}", sum);
    if (length < 0 || (size_t)length >= sizeof(result_body)) {
      status = 500;
      response_body = "{\"error\":\"internal server error\"}";
    } else {
      status = 200;
      response_body = result_body;
    }
  }

  http_response response = {
      status, "application/json", response_body, strlen(response_body)};
  return http_response_send(client_fd, &response, stopping);
}
