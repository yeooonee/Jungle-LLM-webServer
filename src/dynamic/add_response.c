#include "add.h"

#include <stdio.h>
#include <string.h>

#include "http/response.h"

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
