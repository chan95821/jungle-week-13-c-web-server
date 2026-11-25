#define _POSIX_C_SOURCE 200809L
#include "thread_pool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Initialize pool mutex and condition variables (not_full uses CLOCK_MONOTONIC). */
static int init_sync_primitives(struct thread_pool *pool) {
    int rc = pthread_mutex_init(&pool->lock, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }

    rc = pthread_cond_init(&pool->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&pool->lock);
        errno = rc;
        return -1;
    }

    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->lock);
        errno = rc;
        return -1;
    }

    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(&pool->not_full, &attr);
    }
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->lock);
        errno = rc;
        return -1;
    }

    return 0;
}

/* Destroy all synchronization primitives owned by pool. */
static void destroy_sync_primitives(struct thread_pool *pool) {
    pthread_cond_destroy(&pool->not_full);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->lock);
}

/* Wake spawned workers on initialization failure and join all created threads. */
static void stop_and_join_workers(struct thread_pool *pool, size_t spawned_count) {
    pthread_mutex_lock(&pool->lock);
    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < spawned_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }
}

/* Worker thread entry point: dequeue work items and close each client_fd after task_fn. */
static void *worker_main(void *arg) {
    struct thread_pool *pool = arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->count == 0 && !pool->shutting_down) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }

        /* Exit immediately on shutdown so unstarted queued FDs are closed in shutdown(). */
        if (pool->shutting_down) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        struct work_item item = pool->items[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);

        pool->task_fn(item.client_fd, &item.accepted_at, pool->task_context);
        close(item.client_fd);
    }

    return NULL;
}

/* Return true when shutdown or external cancellation has been requested; caller holds lock. */
static bool is_pool_stopping(const struct thread_pool *pool) {
    return pool->shutting_down || (pool->running != NULL && *pool->running == 0);
}

/* Compute an absolute CLOCK_MONOTONIC deadline wait_ms milliseconds in the future. */
static int compute_wait_deadline(struct timespec *deadline, long wait_ms) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) {
        return -1;
    }

    deadline->tv_sec += wait_ms / 1000L;
    deadline->tv_nsec += (wait_ms % 1000L) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }

    return 0;
}

/* Wait while the ring buffer is full, periodically re-checking cancellation; caller holds lock. */
static int wait_until_not_full(struct thread_pool *pool) {
    while (pool->count == pool->capacity) {
        if (is_pool_stopping(pool)) {
            errno = ECANCELED;
            return -1;
        }

        struct timespec deadline;
        if (compute_wait_deadline(&deadline, THREAD_POOL_WAIT_MS) < 0) {
            return -1;
        }

        int rc = pthread_cond_timedwait(&pool->not_full, &pool->lock, &deadline);
        if (rc != 0 && rc != ETIMEDOUT) {
            errno = rc;
            return -1;
        }
    }

    if (is_pool_stopping(pool)) {
        errno = ECANCELED;
        return -1;
    }

    return 0;
}

int thread_pool_init(struct thread_pool *pool, size_t worker_count,
                     size_t queue_capacity,
                     const atomic_int *running,
                     thread_pool_task_fn task_fn, void *task_context) {
    if (pool == NULL || task_fn == NULL || worker_count == 0 || queue_capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(pool, 0, sizeof(*pool));

    pool->items = calloc(queue_capacity, sizeof(*pool->items));
    pool->threads = calloc(worker_count, sizeof(*pool->threads));
    if (pool->items == NULL || pool->threads == NULL) {
        int saved_errno = errno;
        free(pool->threads);
        free(pool->items);
        memset(pool, 0, sizeof(*pool));
        errno = saved_errno;
        return -1;
    }

    if (init_sync_primitives(pool) < 0) {
        int saved_errno = errno;
        free(pool->threads);
        free(pool->items);
        memset(pool, 0, sizeof(*pool));
        errno = saved_errno;
        return -1;
    }

    pool->capacity = queue_capacity;
    pool->thread_count = worker_count;
    pool->running = running;
    pool->task_fn = task_fn;
    pool->task_context = task_context;
    pool->initialized = true;

    for (size_t i = 0; i < worker_count; ++i) {
        int rc = pthread_create(&pool->threads[i], NULL, worker_main, pool);
        if (rc != 0) {
            stop_and_join_workers(pool, i);
            destroy_sync_primitives(pool);
            free(pool->threads);
            free(pool->items);
            memset(pool, 0, sizeof(*pool));
            errno = rc;
            return -1;
        }
    }

    return 0;
}

int thread_pool_submit(struct thread_pool *pool, int client_fd,
                       const struct timespec *accepted_at) {
    if (pool == NULL || !pool->initialized || client_fd < 0 || accepted_at == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->lock);
    if (wait_until_not_full(pool) < 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&pool->lock);
        errno = saved_errno;
        return -1;
    }

    pool->items[pool->tail] = (struct work_item){
        .client_fd = client_fd,
        .accepted_at = *accepted_at
    };
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);

    return 0;
}

void thread_pool_shutdown(struct thread_pool *pool) {
    if (pool == NULL || !pool->initialized) {
        return;
    }

    pthread_mutex_lock(&pool->lock);
    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->thread_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }

    while (pool->count > 0) {
        struct work_item item = pool->items[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        if (item.client_fd >= 0) {
            close(item.client_fd);
        }
    }

    destroy_sync_primitives(pool);
    free(pool->threads);
    free(pool->items);
    memset(pool, 0, sizeof(*pool));
}
