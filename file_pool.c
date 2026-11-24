#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE // Expose O_NOFOLLOW in the macOS SDK.
#endif

#include "file_pool.h"
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FILE_WORKERS = 4 };

struct job_queue {
    struct file_job *head;
    struct file_job *tail;
};

struct file_pool {
    int root_fd;
    int notification[2];
    pthread_t workers[FILE_WORKERS];
    size_t worker_count;
    pthread_mutex_t mutex;
    pthread_cond_t work_available;
    bool stopping;
    struct job_queue pending;
    struct job_queue completed;
    // Only the event loop changes reservations, including files it is sending.
    size_t active_jobs;
    size_t capacity;
};

static void push_job(struct job_queue *queue, struct file_job *job)
{
    job->next = NULL;
    if (queue->tail == NULL) {
        queue->head = job;
    } else {
        queue->tail->next = job;
    }
    queue->tail = job;
}

static struct file_job *pop_job(struct job_queue *queue)
{
    struct file_job *job = queue->head;
    if (job != NULL) {
        queue->head = job->next;
        job->next = NULL;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
    }
    return job;
}

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    return -1;
}

static int decode_path_component(const char *path, size_t length,
                                 char *name)
{
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)path[i];
        if (byte == '%') {
            if (length - i < 3) {
                return 400;
            }
            int high = hex_digit((unsigned char)path[i + 1]);
            int low = hex_digit((unsigned char)path[i + 2]);
            if (high < 0 || low < 0) {
                return 400;
            }
            byte = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (byte == '/' || byte == '\\' || byte < 0x20 || byte == 0x7f) {
            return 403;
        }
        name[used++] = (char)byte;
    }
    name[used] = '\0';

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 403;
    }
    return used == 0 ? 404 : 200;
}

static int file_open_status(int directory, const char *name, int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    case ENOTDIR: {
        struct stat info;
        // O_DIRECTORY may report ENOTDIR for a symlink on some filesystems.
        if (fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISLNK(info.st_mode)) {
            return 403;
        }
        return 404;
    }
    case ENOENT:
    case ENAMETOOLONG:
        return 404;
    default:
        return 500;
    }
}

static const char *file_content_type(const char *name)
{
    static const struct {
        const char *extension;
        const char *content_type;
    } types[] = {
        {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},
        {".js", "text/javascript"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".svg", "image/svg+xml"},
        {".webp", "image/webp"}, {".ico", "image/x-icon"}, {".txt", "text/plain"},
        {".json", "application/json"}, {".pdf", "application/pdf"},
    };
    const char *extension = strrchr(name, '.');
    if (extension != NULL) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (strcasecmp(extension, types[i].extension) == 0) {
                return types[i].content_type;
            }
        }
    }
    return "application/octet-stream";
}

static int open_static_file(struct file_job *job, int root_fd)
{
    const char *path = job->path;
    int directory = root_fd;
    // The request headers already bound the length of each path component.
    char name[HTTP_MAX_HEADERS + 1];
    int status;
    for (;;) {
        size_t length = strcspn(path, "/");
        status = decode_path_component(path, length, name);
        if (status != 200) {
            break;
        }

        bool last = path[length] != '/';
        int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
        if (!last) {
            flags |= O_DIRECTORY;
        }
        int fd = openat(directory, name, flags);
        if (fd == -1) {
            status = file_open_status(directory, name, errno);
            break;
        }
        if (last) {
            struct stat info;
            if (fstat(fd, &info) == -1 || info.st_size < 0) {
                status = 500;
            } else if (!S_ISREG(info.st_mode)) {
                status = 403;
            }
            if (status == 200) {
                job->fd = fd;
                job->remaining = info.st_size;
                job->content_type = file_content_type(name);
            } else {
                close(fd);
            }
            break;
        }
        if (directory != root_fd) {
            close(directory);
        }
        directory = fd;
        path += length + 1;
    }
    if (directory != root_fd) {
        close(directory);
    }
    return status;
}

static void read_file_job(struct file_job *job, int root_fd)
{
    if (job->fd == -1) {
        job->status = open_static_file(job, root_fd);
        if (job->status != 200 || job->remaining == 0) {
            return;
        }
    }

    size_t length = sizeof(job->data);
    if (job->remaining < (off_t)length) {
        length = (size_t)job->remaining;
    }
    ssize_t count;
    do {
        count = read(job->fd, job->data, length);
    } while (count == -1 && errno == EINTR);

    if (count <= 0) {
        job->status = 500;
        return;
    }
    job->length = (size_t)count;
}

static void notify_event_loop(int fd)
{
    const char byte = 0;
    ssize_t count;
    do {
        count = write(fd, &byte, sizeof(byte));
    } while (count == -1 && errno == EINTR);

    // A full pipe already has an unread notification.
    if (count == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("write: file completion");
    }
}

static void *file_worker(void *arg)
{
    struct file_pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->pending.head == NULL && !pool->stopping) {
            pthread_cond_wait(&pool->work_available, &pool->mutex);
        }
        if (pool->stopping) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        struct file_job *job = pop_job(&pool->pending);
        pthread_mutex_unlock(&pool->mutex);

        read_file_job(job, pool->root_fd);

        pthread_mutex_lock(&pool->mutex);
        push_job(&pool->completed, job);
        pthread_mutex_unlock(&pool->mutex);
        notify_event_loop(pool->notification[1]);
    }
}

struct file_pool *file_pool_create(int root_fd, size_t capacity)
{
    struct file_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->root_fd = root_fd;
    pool->capacity = capacity;

    int error;
    if (pipe(pool->notification) == -1) {
        error = errno;
        goto free_pool;
    }
    for (int i = 0; i < 2; i++) {
        int fd = pool->notification[i];
        if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1 ||
            fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
            error = errno;
            goto close_pipe;
        }
    }
    error = pthread_mutex_init(&pool->mutex, NULL);
    if (error != 0) {
        goto close_pipe;
    }
    error = pthread_cond_init(&pool->work_available, NULL);
    if (error != 0) {
        pthread_mutex_destroy(&pool->mutex);
        goto close_pipe;
    }
    for (size_t i = 0; i < FILE_WORKERS; i++) {
        error = pthread_create(&pool->workers[i], NULL, file_worker, pool);
        if (error != 0) {
            file_pool_destroy(pool);
            errno = error;
            return NULL;
        }
        pool->worker_count++;
    }
    return pool;

close_pipe:
    close(pool->notification[0]);
    close(pool->notification[1]);
free_pool:
    free(pool);
    errno = error;
    return NULL;
}

void file_pool_destroy(struct file_pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->work_available);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    struct file_job *job;
    while ((job = pop_job(&pool->pending)) != NULL) {
        file_pool_release(pool, job);
    }
    while ((job = pop_job(&pool->completed)) != NULL) {
        file_pool_release(pool, job);
    }
    pthread_cond_destroy(&pool->work_available);
    pthread_mutex_destroy(&pool->mutex);
    close(pool->notification[0]);
    close(pool->notification[1]);
    free(pool);
}

int file_pool_notification_fd(const struct file_pool *pool)
{
    return pool->notification[0];
}

int file_pool_drain_notifications(struct file_pool *pool)
{
    char bytes[64];
    for (;;) {
        ssize_t count = read(pool->notification[0], bytes, sizeof(bytes));
        if (count > 0 || (count == -1 && errno == EINTR)) {
            continue;
        }
        if (count == 0) {
            errno = EPIPE;
            return -1;
        }
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
}

int file_pool_submit_open(struct file_pool *pool, int slot, uint64_t connection_id,
                          const char *path)
{
    if (pool->active_jobs == pool->capacity) {
        return 503;
    }
    size_t length = strcspn(path, "?");
    struct file_job *job = calloc(1, sizeof(*job) + length + 1);
    if (job == NULL) {
        return 500;
    }
    memcpy(job->path, path, length);
    job->slot = slot;
    job->connection_id = connection_id;
    job->fd = -1;
    pool->active_jobs++;
    file_pool_submit_read(pool, job);
    return 0;
}

void file_pool_submit_read(struct file_pool *pool, struct file_job *job)
{
    pthread_mutex_lock(&pool->mutex);
    push_job(&pool->pending, job);
    pthread_cond_signal(&pool->work_available);
    pthread_mutex_unlock(&pool->mutex);
}

struct file_job *file_pool_take_completed(struct file_pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    struct file_job *job = pop_job(&pool->completed);
    pthread_mutex_unlock(&pool->mutex);
    return job;
}

void file_pool_release(struct file_pool *pool, struct file_job *job)
{
    if (job->fd != -1) {
        close(job->fd);
    }
    free(job);
    pool->active_jobs--;
}
