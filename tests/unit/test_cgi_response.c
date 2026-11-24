#include "cgi/response.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char output[] = "Status: 400 Bad Request\r\n"
                        "Content-Type: application/json\r\n\r\n"
                        "{\"error\":\"bad input\"}";
  cgi_response response;
  assert(cgi_response_parse(output, sizeof(output) - 1, &response) == 1);
  assert(response.status == 400);
  assert(response.content_type.length == strlen("application/json"));
  assert(memcmp(response.content_type.data, "application/json",
                response.content_type.length) == 0);
  assert(response.body.length == strlen("{\"error\":\"bad input\"}"));
  assert(response.body.data == strstr(output, "{\"error\""));

  const char binary[] = "content-type:\t application/octet-stream \t\r\n"
                        "X-Note: a:b:c\r\n\r\nA\0B\r\n\r\nC";
  assert(cgi_response_parse(binary, sizeof(binary) - 1, &response) == 1);
  assert(response.status == 200 && response.body.length == 8);
  assert(memcmp(response.body.data, "A\0B\r\n\r\nC", 8) == 0);

  const char empty[] = "Content-Type: text/plain\r\nsTaTuS: 201 Created\r\n\r\n";
  assert(cgi_response_parse(empty, sizeof(empty) - 1, &response) == 1);
  assert(response.status == 201 && response.body.length == 0);

  const char *invalid[] = {
      "", "\r\n", "Content-Type: text/plain\r\n",
      "Content-Type: text/plain\n\nhello", "Content-Type: text/plain\rX\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n",
      "Status: 200 OK\r\n\r\n", "Content-Type: \t\r\n\r\n",
      "Content-Type : text/plain\r\n\r\n", ": value\r\n\r\n",
      "Content-Type: text/plain\r\nContent-Type: text/plain\r\n\r\n",
      "Content-Type: text/plain\r\nStatus: 200 OK\r\nStatus: 200 OK\r\n\r\n",
      "Content-Type: text/plain\r\nStatus: 99 Bad\r\n\r\n",
      "Content-Type: text/plain\r\nStatus: 600 Bad\r\n\r\n",
      "Content-Type: text/plain\r\nStatus: 1x0 Bad\r\n\r\n",
      "Content-Type: text/plain\r\nStatus: 200\r\n\r\n",
      "Content-Type: text/plain\r\nStatus: 200 \r\n\r\n",
      "Content-Type: text/plain\r\nStatus: 200\tOK\r\n\r\n",
      "Content-Type: text/\tplain\r\n\r\n",
      "Content-Type: text/plain\r\nX: \177\r\n\r\n",
      "Content-Type: text/plain\r\n folded: value\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    response = (cgi_response){.status = 777};
    assert(cgi_response_parse(invalid[i], strlen(invalid[i]), &response) == 0);
    assert(response.status == 777 && response.body.data == NULL);
  }
  const char nul_header[] = "Content-Type: text/plain\0x\r\n\r\n";
  assert(cgi_response_parse(nul_header, sizeof(nul_header) - 1, &response) == 0);
  assert(cgi_response_parse(NULL, 0, &response) == 0);
  assert(cgi_response_parse(output, sizeof(output) - 1, NULL) == 0);

  size_t capacity = HTTP_MAX_HEADER_BYTES + HTTP_MAX_BODY_BYTES + 1;
  char *large = malloc(capacity);
  assert(large != NULL);
  const char prefix[] = "Content-Type: text/plain\r\nX: ";
  size_t prefix_length = sizeof(prefix) - 1;
  memcpy(large, prefix, prefix_length);
  memset(large + prefix_length, 'a', HTTP_MAX_HEADER_BYTES - prefix_length - 4);
  memcpy(large + HTTP_MAX_HEADER_BYTES - 4, "\r\n\r\n", 4);
  memset(large + HTTP_MAX_HEADER_BYTES, 0, HTTP_MAX_BODY_BYTES + 1);
  assert(cgi_response_parse(large, capacity - 1, &response) == 1);
  assert(response.body.length == HTTP_MAX_BODY_BYTES);
  assert(cgi_response_parse(large, capacity, &response) == 0);
  memmove(large + HTTP_MAX_HEADER_BYTES - 3,
          large + HTTP_MAX_HEADER_BYTES - 4, 4);
  large[HTTP_MAX_HEADER_BYTES - 4] = 'a';
  assert(cgi_response_parse(large, HTTP_MAX_HEADER_BYTES + 1, &response) == 0);

  size_t used = strlen("Content-Type: text/plain\r\n");
  memcpy(large, "Content-Type: text/plain\r\n", used);
  for (size_t i = 1; i < HTTP_MAX_HEADERS; ++i) {
    memcpy(large + used, "X: a\r\n", 6);
    used += 6;
  }
  memcpy(large + used, "\r\n", 2);
  assert(cgi_response_parse(large, used + 2, &response) == 1);
  memcpy(large + used, "X: a\r\n\r\n", 8);
  assert(cgi_response_parse(large, used + 8, &response) == 0);
  free(large);
  puts("CGI response parser checks passed");
  return 0;
}
