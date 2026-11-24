#include "cgi/run.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <cgi-path> <body-length>\n", argv[0]);
    return EXIT_FAILURE;
  }

  size_t length = 0;
  if (argv[2][0] == '\0') {
    fprintf(stderr, "Invalid body length\n");
    return EXIT_FAILURE;
  }
  for (size_t i = 0; argv[2][i] != '\0'; ++i) {
    unsigned char c = (unsigned char)argv[2][i];
    if (c < '0' || c > '9' ||
        length > (HTTP_MAX_BODY_BYTES - (size_t)(c - '0')) / 10) {
      fprintf(stderr, "Invalid body length\n");
      return EXIT_FAILURE;
    }
    length = length * 10 + (size_t)(c - '0');
  }

  char *body = malloc(length == 0 ? 1 : length);
  if (body == NULL) {
    perror("allocate body");
    return EXIT_FAILURE;
  }
  size_t received = fread(body, 1, length, stdin);
  if (received != length) {
    int input_error = ferror(stdin);
    free(body);
    if (input_error) {
      perror("read body");
      return EXIT_FAILURE;
    }
    if (printf("Status: 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
               "{\"error\":\"incomplete body\"}") < 0 || fflush(stdout) == EOF) {
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }
  struct sigaction action = {0};
  action.sa_handler = SIG_IGN;
  if (sigemptyset(&action.sa_mask) == -1 ||
      sigaction(SIGPIPE, &action, NULL) == -1) {
    perror("sigaction");
    free(body);
    return EXIT_FAILURE;
  }

  cgi_output output;
  int result = cgi_run(argv[1], (http_slice){body, length}, &output, NULL, -1);
  free(body);
  if (result != 0) {
    return result > 0 ? result : EXIT_FAILURE;
  }
  /* shortcut: 학습용 stdout 쓰기는 blocking이다. 서버 연결 시 송신 기한을 관리한다. */
  if (fwrite(output.data, 1, output.length, stdout) != output.length ||
      fflush(stdout) == EOF) {
    perror("write CGI response");
    result = EXIT_FAILURE;
  }
  free(output.data);
  return result;
}
