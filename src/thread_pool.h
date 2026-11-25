#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Constants bounding the fixed-size worker pool and request queue. */
enum {
    THREAD_POOL_DEFAULT_WORKERS = 4,
    THREAD_POOL_MAX_WORKERS = 64,
    THREAD_POOL_QUEUE_CAPACITY = 128,
    THREAD_POOL_WAIT_MS = 100
};

/* Worker callback invoked for each accepted connection; pool closes client_fd after return. */
typedef void (*thread_pool_task_fn)(int client_fd,
                                    const struct timespec *accepted_at,
                                    void *context);

/* Queued client connection paired with its monotonic accept timestamp. */
struct work_item {
    int client_fd;
    struct timespec accepted_at;
};

/* Bounded-queue blocking thread pool synchronized by a mutex and two condvars. */
struct thread_pool {
    struct work_item *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    bool shutting_down;
    bool initialized;

    pthread_t *threads;
    size_t thread_count;

    const atomic_int *running;
    thread_pool_task_fn task_fn;
    void *task_context;
};

/* Initialize queue, synchronization primitives, and worker_count worker threads.
 * Returns 0 on success, or -1 with errno set on invalid arguments or resource failure. */
int thread_pool_init(struct thread_pool *pool, size_t worker_count,
                     size_t queue_capacity,
                     const atomic_int *running,
                     thread_pool_task_fn task_fn, void *task_context);

/* Enqueue client_fd and accepted_at, waiting while full up to THREAD_POOL_WAIT_MS intervals.
 * Transfers client_fd ownership on 0; returns -1 (ECANCELED/EINVAL/error) with caller owning FD. */
int thread_pool_submit(struct thread_pool *pool, int client_fd,
                       const struct timespec *accepted_at);

/* Signal workers to exit, join active workers, close unstarted queued FDs, and free resources. */
void thread_pool_shutdown(struct thread_pool *pool);

#endif
