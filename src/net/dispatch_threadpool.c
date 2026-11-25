#include "dispatch.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

struct worker_state {
  pthread_t thread;
  int client_fd;
};

static struct {
  pthread_mutex_t mutex;    /* 큐와 상태를 보호하는 뮤텍스 */
  pthread_cond_t available; /* 빈 큐에서 기다리던 worker를 깨우는 조건 변수 */
  struct worker_state workers[THREAD_POOL_WORKERS];
  int queue[THREAD_POOL_QUEUE_CAPACITY]; /* worker가 맡을 connect fd 보관하는
                                            원형 */
  size_t head;                           /* 다음에 꺼낼 위치 저장 */
  size_t count;                          /* 빈 큐, 가득 찬 큐를 구분 */
  size_t started;
  int root_fd;
  const char *cgi_program;
  proxy_routes *routes;
  int cancel_pipe[2];
  int stopping;
} pool;

static void *worker(void *argument) {
  struct worker_state *worker = argument;
  /* 소켓은 shutdown, CGI 대기는 취소 파이프로 깨워 공유 종료 플래그를 피한다. */
  const volatile sig_atomic_t not_stopping = 0;
  for (;;) {
    pthread_mutex_lock(&pool.mutex);
    while (pool.count == 0 && !pool.stopping) {
      pthread_cond_wait(&pool.available, &pool.mutex);
    }
    if (pool.stopping) {
      pthread_mutex_unlock(&pool.mutex);
      return NULL;
    }
    int client_fd = pool.queue[pool.head];
    pool.head = (pool.head + 1) % THREAD_POOL_QUEUE_CAPACITY;
    --pool.count;
    worker->client_fd = client_fd;
    pthread_mutex_unlock(&pool.mutex);

    int result = tcp_handle_client(client_fd, pool.root_fd, pool.cgi_program, pool.routes,
                                     pool.cancel_pipe[0], &not_stopping);
    int error = errno;
    pthread_mutex_lock(&pool.mutex);
    int report_error = result == -1 && !pool.stopping;
    /* 종료 스레드가 재사용된 fd에 shutdown하지 않도록 잠금 안에서 정리한다. */
    close(client_fd);
    worker->client_fd = -1;
    pthread_mutex_unlock(&pool.mutex);
    if (report_error) {
      errno = error;
      perror("client I/O");
    }
  }
}

void dispatch_client(int client_fd) {
  pthread_mutex_lock(&pool.mutex);
  if (pool.stopping || pool.count == THREAD_POOL_QUEUE_CAPACITY) {
    pthread_mutex_unlock(&pool.mutex);
    close(client_fd);
    return;
  }
  size_t tail = (pool.head + pool.count) % THREAD_POOL_QUEUE_CAPACITY;
  pool.queue[tail] = client_fd;
  ++pool.count;
  pthread_cond_signal(&pool.available);
  pthread_mutex_unlock(&pool.mutex);
}

void dispatch_stop(void) {
  pthread_mutex_lock(&pool.mutex);
  pool.stopping = 1;
  /* 읽기 끝은 모든 작업자가 공유하며 쓰기 끝을 닫으면 모두 EOF/HUP을 본다. */
  close(pool.cancel_pipe[1]);
  while (pool.count > 0) {
    close(pool.queue[pool.head]);
    pool.head = (pool.head + 1) % THREAD_POOL_QUEUE_CAPACITY;
    --pool.count;
  }
  for (size_t i = 0; i < pool.started; ++i) {
    if (pool.workers[i].client_fd != -1) {
      shutdown(pool.workers[i].client_fd, SHUT_RDWR);
    }
  }
  pthread_cond_broadcast(&pool.available);
  pthread_mutex_unlock(&pool.mutex);
  for (size_t i = 0; i < pool.started; ++i) {
    pthread_join(pool.workers[i].thread, NULL);
  }
  close(pool.cancel_pipe[0]);
  pthread_cond_destroy(&pool.available);
  pthread_mutex_destroy(&pool.mutex);
}

int dispatch_start(int root_fd, const char *cgi_program,
                     proxy_routes *routes,
                     const volatile sig_atomic_t *stopping) {
  (void)stopping;
  pool.head = 0;
  pool.count = 0;
  pool.started = 0;
  pool.stopping = 0;
  pool.root_fd = root_fd;
  pool.cgi_program = cgi_program;
  pool.routes = routes;
  for (size_t i = 0; i < THREAD_POOL_WORKERS; ++i) {
    pool.workers[i].client_fd = -1;
  }
  int error = pthread_mutex_init(&pool.mutex, NULL);
  if (error != 0) {
    errno = error;
    return -1;
  }
  error = pthread_cond_init(&pool.available, NULL);
  if (error != 0) {
    pthread_mutex_destroy(&pool.mutex);
    errno = error;
    return -1;
  }

  if (pipe(pool.cancel_pipe) == -1) {
    int saved_error = errno;
    pthread_cond_destroy(&pool.available);
    pthread_mutex_destroy(&pool.mutex);
    errno = saved_error;
    return -1;
  }

  /* 작업자에게 마스크를 상속시켜 종료 시그널은 수락 스레드에서만 처리한다. */
  sigset_t signals;
  sigset_t original_mask;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  error = pthread_sigmask(SIG_BLOCK, &signals, &original_mask);
  if (error != 0) {
    close(pool.cancel_pipe[0]);
    close(pool.cancel_pipe[1]);
    pthread_cond_destroy(&pool.available);
    pthread_mutex_destroy(&pool.mutex);
    errno = error;
    return -1;
  }
  for (size_t i = 0; i < THREAD_POOL_WORKERS; ++i) {
    error =
        pthread_create(&pool.workers[i].thread, NULL, worker, &pool.workers[i]);
    if (error != 0) {
      break;
    }
    ++pool.started;
  }
  int mask_error = pthread_sigmask(SIG_SETMASK, &original_mask, NULL);
  if (error != 0 || mask_error != 0) {
    dispatch_stop();
    errno = error != 0 ? error : mask_error;
    return -1;
  }
  return 0;
}
