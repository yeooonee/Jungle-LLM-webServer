// CGI 예제: a=1&b=2 → 3. GET은 QUERY_STRING, POST는 stdin(본문)에서 읽음 (CSAPP Tiny adder 방식)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char input[1024] = "";
    const char *method = getenv("REQUEST_METHOD");
    if (method && !strcmp(method, "POST")) {
        long len = strtol(getenv("CONTENT_LENGTH") ? getenv("CONTENT_LENGTH") : "0", NULL, 10);
        if (len >= (long)sizeof(input)) len = sizeof(input) - 1;
        input[fread(input, 1, len, stdin)] = '\0';   // 본문은 서버가 파이프로 넣어 줌
    } else if (getenv("QUERY_STRING")) {
        snprintf(input, sizeof(input), "%s", getenv("QUERY_STRING"));
    }

    int a = 0, b = 0;
    if (sscanf(input, "a=%d&b=%d", &a, &b) != 2) {
        fprintf(stderr, "bad input: [%s]\n", input);   // → 서버의 cgi.log
        printf("Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nusage: ?a=1&b=2\n");
        return 0;
    }
    // CGI 표준 출력: 헤더 + 빈 줄 + 본문. 상태 줄(HTTP/1.1 200 OK)은 서버가 붙임
    printf("Content-Type: text/plain\r\n\r\n%d + %d = %d\n", a, b, a + b);
    return 0;
}
