// 스레드 풀: 메인 루프(생산자)가 연결 fd를 큐에 넣고, 워커(소비자)가 꺼내 처리
// 큐는 원형 배열. 여러 스레드가 같이 만지니 mutex로 한 번에 한 스레드만,
// 비었거나 꽉 찼을 때는 조건 변수로 재워서 바쁜 대기(계속 확인하며 CPU 낭비)를 막음
#include <pthread.h>
#include <stdlib.h>

#include "pool.h"

static int *queue;            // 연결 fd들
static int cap, head, count;  // 크기, 꺼낼 위치, 들어 있는 개수
static void (*job)(int);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;   // 워커가 기다림
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;    // 메인 루프가 기다림

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (count == 0)                            // while: 깨어나도 다른 워커가 먼저 가져갔을 수 있음
            pthread_cond_wait(&not_empty, &lock);     // 잠그개를 풀고 잠듦 → 깨면 다시 잠그고 돌아옴
        int fd = queue[head];
        head = (head + 1) % cap;
        count--;
        pthread_cond_signal(&not_full);               // 자리 하나 남 → 메인 루프 깨우기
        pthread_mutex_unlock(&lock);

        job(fd);                                      // 처리는 잠금 밖에서 (안 그러면 한 번에 한 명만 처리)
    }
    return NULL;
}

int pool_start(int workers, int queue_size, void (*fn)(int)) {
    queue = malloc(sizeof(int) * queue_size);
    if (!queue) return -1;
    cap = queue_size;
    job = fn;
    for (int i = 0; i < workers; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) != 0) return -1;
        pthread_detach(t);                            // 서버가 끝날 때까지 돌므로 join 안 함
    }
    return 0;
}

void pool_push(int fd) {
    pthread_mutex_lock(&lock);
    while (count == cap)
        pthread_cond_wait(&not_full, &lock);          // 큐 꽉 참 → accept도 멈춤 (손님은 OS listen 대기열에)
    queue[(head + count) % cap] = fd;
    count++;
    pthread_cond_signal(&not_empty);                  // 일감 생김 → 워커 하나 깨우기
    pthread_mutex_unlock(&lock);
}
