// 테스트용 CGI: 1초 자고 응답 (동시성 테스트의 '느린 동적 요청' 역할)
#include <stdio.h>
#include <unistd.h>

int main(void) {
    sleep(1);
    printf("Content-Type: text/plain\r\n\r\nslept 1s\n");
    return 0;
}
