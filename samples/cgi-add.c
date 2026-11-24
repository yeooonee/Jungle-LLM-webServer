#include "dynamic/add.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_content_length(size_t *length) {
  const char *value = getenv("CONTENT_LENGTH");
  if (value == NULL || *value == '\0') {
    return 0;
  }
  size_t parsed = 0;
  for (size_t i = 0; value[i] != '\0'; ++i) {
    unsigned char c = (unsigned char)value[i];
    if (c < '0' || c > '9') {
      return 0;
    }
    size_t digit = c - '0';
    if (parsed > (HTTP_MAX_BODY_BYTES - digit) / 10) {
      return 0;
    }
    parsed = parsed * 10 + digit;
  }
  *length = parsed;
  return 1;
}

static int respond(const char *status, const char *body) {
  if (printf("Status: %s\r\nContent-Type: application/json\r\n\r\n%s",
             status, body) < 0 || fflush(stdout) == EOF) {
    perror("CGI output");
    return EXIT_FAILURE;
  }
  /* HTTP 상태 400도 정상적으로 생성한 CGI 응답이므로 실행 성공으로 반환한다. */
  return EXIT_SUCCESS;
}

int main(void) {
  const char *method = getenv("REQUEST_METHOD");
  size_t length;
  if (method == NULL || strcmp(method, "POST") != 0 ||
      !read_content_length(&length)) {
    return respond("400 Bad Request", "{\"error\":\"invalid CGI request\"}");
  }

  char *body = malloc(length == 0 ? 1 : length);
  if (body == NULL) {
    perror("CGI input allocation");
    return respond("500 Internal Server Error", "{\"error\":\"internal server error\"}");
  }
  size_t received = fread(body, 1, length, stdin);
  if (received != length) {
    int input_error = ferror(stdin);
    free(body);
    if (input_error) {
      perror("CGI input");
      return respond("500 Internal Server Error", "{\"error\":\"input read failed\"}");
    }
    return respond("400 Bad Request", "{\"error\":\"incomplete body\"}");
  }

  long left;
  long right;
  long sum;
  int valid = dynamic_add_parse(body, length, &left, &right) &&
              dynamic_add(left, right, &sum);
  free(body);
  if (!valid) {
    return respond("400 Bad Request", "{\"error\":\"invalid input or sum out of range\"}");
  }

  char result_body[128];
  int result_length = snprintf(result_body, sizeof(result_body), "{\"result\":%ld}", sum);
  if (result_length < 0 || (size_t)result_length >= sizeof(result_body)) {
    return respond("500 Internal Server Error", "{\"error\":\"internal server error\"}");
  }
  return respond("200 OK", result_body);
}
