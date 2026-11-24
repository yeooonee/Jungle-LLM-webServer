// 스레드 풀: 워커 N개 + 고정 크기 작업 큐 (작업 = 연결 fd 하나)
#ifndef POOL_H
#define POOL_H

// 워커를 만들어 대기시킴. 워커는 큐에서 fd를 꺼내 job(fd)를 호출. 실패 -1
int pool_start(int workers, int queue_size, void (*job)(int fd));

// 큐에 fd 넣기. 큐가 꽉 차면 자리가 날 때까지 기다림 (→ 그동안 메인 루프는 accept 안 함)
void pool_push(int fd);

#endif
