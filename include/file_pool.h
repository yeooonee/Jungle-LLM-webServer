#ifndef FILE_POOL_H
#define FILE_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { FILE_CHUNK_SIZE = 4096 };

struct file_pool;

// A job and its open file move between the pool and the event loop together.
struct file_job {
    struct file_job *next;
    int slot;
    uint64_t connection_id;
    int fd;
    int status;
    off_t remaining;
    const char *content_type;
    size_t length;
    char data[FILE_CHUNK_SIZE];
    char path[];
};

struct file_pool *file_pool_create(int root_fd, size_t capacity);
void file_pool_destroy(struct file_pool *pool);
int file_pool_notification_fd(const struct file_pool *pool);
int file_pool_drain_notifications(struct file_pool *pool);

// The event loop calls these functions. Opening returns 0 or an HTTP error status.
int file_pool_submit_open(struct file_pool *pool, int slot, uint64_t connection_id,
                          const char *path);
void file_pool_submit_read(struct file_pool *pool, struct file_job *job);
struct file_job *file_pool_take_completed(struct file_pool *pool);
void file_pool_release(struct file_pool *pool, struct file_job *job);

#endif
