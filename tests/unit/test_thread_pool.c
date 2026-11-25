#define _POSIX_C_SOURCE 200809L

#include "../../src/thread_pool.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_test_running = 1;

static void on_sigusr1(int sig) {
    (void)sig;
    g_test_running = 0;
}

static void sleep_ms(long ms) {
    struct timespec req = {
        .tv_sec = ms / 1000L,
        .tv_nsec = (ms % 1000L) * 1000000L
    };
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

static long elapsed_ms_since(const struct timespec *start) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    long sec_diff = (long)(now.tv_sec - start->tv_sec);
    long nsec_diff = now.tv_nsec - start->tv_nsec;
    return sec_diff * 1000L + nsec_diff / 1000000L;
}

static int count_open_fds(void) {
    DIR *dir = opendir("/proc/self/fd");
    assert(dir != NULL);

    int dfd = dirfd(dir);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *endptr = NULL;
        long fd_num = strtol(entry->d_name, &endptr, 10);
        if (*entry->d_name != '\0' && *endptr == '\0' && fd_num == (long)dfd) {
            continue;
        }
        count++;
    }

    assert(closedir(dir) == 0);
    return count;
}

static void make_tagged_pipe(int pipefd[2], size_t id) {
    assert(pipe(pipefd) == 0);
    ssize_t written = write(pipefd[1], &id, sizeof(id));
    assert(written == (ssize_t)sizeof(id));
    assert(close(pipefd[1]) == 0);
}

static size_t read_pipe_tag(int fd) {
    size_t id = (size_t)-1;
    unsigned char *dst = (unsigned char *)&id;
    size_t offset = 0;

    while (offset < sizeof(id)) {
        ssize_t n = read(fd, dst + offset, sizeof(id) - offset);
        assert(n > 0);
        offset += (size_t)n;
    }

    return id;
}

static void assert_fd_closed(int fd) {
    errno = 0;
    assert(fcntl(fd, F_GETFD) == -1);
    assert(errno == EBADF);
}

static void noop_task(int client_fd, const struct timespec *accepted_at, void *context) {
    (void)client_fd;
    (void)accepted_at;
    (void)context;
}

/* --- 1. Argument validation and immediate init/shutdown --- */

static void test_invalid_args_and_immediate_shutdown(void) {
    int fd_before = count_open_fds();
    struct thread_pool pool = {0};
    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};

    errno = 0;
    assert(thread_pool_init(NULL, 4, 8, NULL, noop_task, NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(thread_pool_init(&pool, 4, 8, NULL, NULL, NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(thread_pool_init(&pool, 0, 8, NULL, noop_task, NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(thread_pool_init(&pool, 4, 0, NULL, noop_task, NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(thread_pool_submit(NULL, 0, &ts) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(thread_pool_submit(&pool, 0, &ts) == -1);
    assert(errno == EINVAL);

    thread_pool_shutdown(NULL);
    thread_pool_shutdown(&pool);

    assert(thread_pool_init(&pool, 4, 8, NULL, noop_task, NULL) == 0);

    errno = 0;
    assert(thread_pool_submit(&pool, -1, &ts) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(thread_pool_submit(&pool, 0, NULL) == -1);
    assert(errno == EINVAL);

    thread_pool_shutdown(&pool);
    thread_pool_shutdown(&pool);
    assert(count_open_fds() == fd_before);
}

/* --- 2. Exact-once execution & FD ownership across workers --- */

struct exact_once_ctx {
    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    size_t target_count;
    size_t completed_count;
    int *seen;
};

static void exact_once_task(int client_fd, const struct timespec *accepted_at, void *context) {
    struct exact_once_ctx *ctx = context;
    assert(fcntl(client_fd, F_GETFD) != -1);

    size_t id = read_pipe_tag(client_fd);
    assert(id < ctx->target_count);
    assert(accepted_at->tv_sec == 100 + (time_t)id);
    assert(accepted_at->tv_nsec == (long)id * 1000L);

    pthread_mutex_lock(&ctx->lock);
    assert(ctx->seen[id] == 0);
    ctx->seen[id]++;
    ctx->completed_count++;
    if (ctx->completed_count == ctx->target_count) {
        pthread_cond_signal(&ctx->done_cond);
    }
    pthread_mutex_unlock(&ctx->lock);
}

struct producer_args {
    struct thread_pool *pool;
    int *read_fds;
    size_t start;
    size_t end;
};

static void *producer_main(void *arg) {
    struct producer_args *args = arg;
    for (size_t i = args->start; i < args->end; ++i) {
        struct timespec ts = {.tv_sec = 100 + (time_t)i, .tv_nsec = (long)i * 1000L};
        assert(thread_pool_submit(args->pool, args->read_fds[i], &ts) == 0);
    }
    return NULL;
}

static void run_exact_once_scenario(size_t producer_count, size_t items_per_producer,
                                    size_t workers, size_t capacity) {
    int fd_before = count_open_fds();
    size_t total = producer_count * items_per_producer;
    int *read_fds = calloc(total, sizeof(*read_fds));
    int *seen = calloc(total, sizeof(*seen));
    assert(read_fds != NULL && seen != NULL);

    for (size_t i = 0; i < total; ++i) {
        int pipefd[2];
        make_tagged_pipe(pipefd, i);
        read_fds[i] = pipefd[0];
    }

    struct exact_once_ctx ctx = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .done_cond = PTHREAD_COND_INITIALIZER,
        .target_count = total,
        .completed_count = 0,
        .seen = seen
    };

    struct thread_pool pool;
    assert(thread_pool_init(&pool, workers, capacity, NULL, exact_once_task, &ctx) == 0);

    pthread_t *producers = calloc(producer_count, sizeof(*producers));
    struct producer_args *pargs = calloc(producer_count, sizeof(*pargs));
    assert(producers != NULL && pargs != NULL);

    for (size_t p = 0; p < producer_count; ++p) {
        pargs[p] = (struct producer_args){
            .pool = &pool,
            .read_fds = read_fds,
            .start = p * items_per_producer,
            .end = (p + 1) * items_per_producer
        };
        assert(pthread_create(&producers[p], NULL, producer_main, &pargs[p]) == 0);
    }

    for (size_t p = 0; p < producer_count; ++p) {
        assert(pthread_join(producers[p], NULL) == 0);
    }

    pthread_mutex_lock(&ctx.lock);
    while (ctx.completed_count < total) {
        pthread_cond_wait(&ctx.done_cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    thread_pool_shutdown(&pool);

    for (size_t i = 0; i < total; ++i) {
        assert(seen[i] == 1);
        assert_fd_closed(read_fds[i]);
    }

    pthread_cond_destroy(&ctx.done_cond);
    pthread_mutex_destroy(&ctx.lock);
    free(pargs);
    free(producers);
    free(seen);
    free(read_fds);

    assert(count_open_fds() == fd_before);
}

static void test_exact_once_and_fd_ownership(void) {
    run_exact_once_scenario(1, 64, 4, 16);
    run_exact_once_scenario(4, 32, 4, 16);
}

/* --- 3. Backpressure & cond_timedwait tests --- */

struct gate_ctx {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool gate_open;
    size_t started_count;
    size_t finished_count;
    int seen[8];
};

static void gated_task(int client_fd, const struct timespec *accepted_at, void *context) {
    (void)accepted_at;
    struct gate_ctx *ctx = context;
    size_t id = read_pipe_tag(client_fd);
    assert(id < 8);

    pthread_mutex_lock(&ctx->lock);
    ctx->started_count++;
    pthread_cond_broadcast(&ctx->cond);
    while (!ctx->gate_open) {
        pthread_cond_wait(&ctx->cond, &ctx->lock);
    }
    ctx->seen[id]++;
    ctx->finished_count++;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

struct blocked_submit_args {
    struct thread_pool *pool;
    int fd;
    pthread_mutex_t lock;
    bool done;
    int rc;
    int err;
    long elapsed_ms;
};

static void *blocked_submit_main(void *arg) {
    struct blocked_submit_args *args = arg;
    struct timespec start;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);

    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    int rc = thread_pool_submit(args->pool, args->fd, &ts);
    int err = errno;
    long elapsed = elapsed_ms_since(&start);

    pthread_mutex_lock(&args->lock);
    args->rc = rc;
    args->err = err;
    args->elapsed_ms = elapsed;
    args->done = true;
    pthread_mutex_unlock(&args->lock);
    return NULL;
}

static void test_backpressure_unblock_on_worker_finish(void) {
    int fd_before = count_open_fds();
    int read_fds[4];
    for (size_t i = 0; i < 4; ++i) {
        int pipefd[2];
        make_tagged_pipe(pipefd, i);
        read_fds[i] = pipefd[0];
    }

    struct gate_ctx ctx = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .gate_open = false
    };

    struct thread_pool pool;
    assert(thread_pool_init(&pool, 1, 2, NULL, gated_task, &ctx) == 0);

    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    assert(thread_pool_submit(&pool, read_fds[0], &ts) == 0);

    pthread_mutex_lock(&ctx.lock);
    while (ctx.started_count < 1) {
        pthread_cond_wait(&ctx.cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    assert(thread_pool_submit(&pool, read_fds[1], &ts) == 0);
    assert(thread_pool_submit(&pool, read_fds[2], &ts) == 0);

    struct blocked_submit_args sub = {
        .pool = &pool,
        .fd = read_fds[3],
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .done = false
    };
    pthread_t submit_thread;
    assert(pthread_create(&submit_thread, NULL, blocked_submit_main, &sub) == 0);

    sleep_ms(30);
    pthread_mutex_lock(&sub.lock);
    assert(sub.done == false);
    pthread_mutex_unlock(&sub.lock);

    pthread_mutex_lock(&ctx.lock);
    ctx.gate_open = true;
    pthread_cond_broadcast(&ctx.cond);
    pthread_mutex_unlock(&ctx.lock);

    assert(pthread_join(submit_thread, NULL) == 0);
    assert(sub.rc == 0);

    pthread_mutex_lock(&ctx.lock);
    while (ctx.finished_count < 4) {
        pthread_cond_wait(&ctx.cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    thread_pool_shutdown(&pool);

    for (size_t i = 0; i < 4; ++i) {
        assert(ctx.seen[i] == 1);
        assert_fd_closed(read_fds[i]);
    }

    pthread_mutex_destroy(&sub.lock);
    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.lock);
    assert(count_open_fds() == fd_before);
}

struct signal_stopper_args {
    pthread_t target_thread;
    long delay_ms;
};

static void *signal_stopper_main(void *arg) {
    struct signal_stopper_args *args = arg;
    sleep_ms(args->delay_ms);
    assert(pthread_kill(args->target_thread, SIGUSR1) == 0);
    return NULL;
}

static void test_backpressure_running_flag_escape(void) {
    int fd_before = count_open_fds();

    struct sigaction sa = {0};
    struct sigaction old_sa = {0};
    sa.sa_handler = on_sigusr1;
    assert(sigemptyset(&sa.sa_mask) == 0);
    assert(sigaction(SIGUSR1, &sa, &old_sa) == 0);
    g_test_running = 1;

    int read_fds[4];
    for (size_t i = 0; i < 4; ++i) {
        int pipefd[2];
        make_tagged_pipe(pipefd, i);
        read_fds[i] = pipefd[0];
    }

    struct gate_ctx ctx = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .gate_open = false
    };

    struct thread_pool pool;
    assert(thread_pool_init(&pool, 1, 2, &g_test_running, gated_task, &ctx) == 0);

    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    assert(thread_pool_submit(&pool, read_fds[0], &ts) == 0);

    pthread_mutex_lock(&ctx.lock);
    while (ctx.started_count < 1) {
        pthread_cond_wait(&ctx.cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    assert(thread_pool_submit(&pool, read_fds[1], &ts) == 0);
    assert(thread_pool_submit(&pool, read_fds[2], &ts) == 0);

    struct signal_stopper_args stopper_args = {
        .target_thread = pthread_self(),
        .delay_ms = 30
    };
    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, signal_stopper_main, &stopper_args) == 0);

    struct timespec start;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    errno = 0;
    int rc = thread_pool_submit(&pool, read_fds[3], &ts);
    int err = errno;
    long elapsed = elapsed_ms_since(&start);

    assert(rc == -1);
    assert(err == ECANCELED);
    assert(elapsed <= 250);

    /* Unsubmitted FD remains owned by caller and still open until caller closes it. */
    assert(fcntl(read_fds[3], F_GETFD) != -1);

    /* Open gate so queue drains/clears, then confirm submit still rejects when running == 0. */
    pthread_mutex_lock(&ctx.lock);
    ctx.gate_open = true;
    pthread_cond_broadcast(&ctx.cond);
    pthread_mutex_unlock(&ctx.lock);

    pthread_mutex_lock(&ctx.lock);
    while (ctx.finished_count < 3) {
        pthread_cond_wait(&ctx.cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    errno = 0;
    assert(thread_pool_submit(&pool, read_fds[3], &ts) == -1);
    assert(errno == ECANCELED);
    assert(fcntl(read_fds[3], F_GETFD) != -1);
    assert(close(read_fds[3]) == 0);

    assert(pthread_join(stopper, NULL) == 0);

    thread_pool_shutdown(&pool);

    for (size_t i = 0; i < 3; ++i) {
        assert_fd_closed(read_fds[i]);
    }

    assert(sigaction(SIGUSR1, &old_sa, NULL) == 0);
    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.lock);
    assert(count_open_fds() == fd_before);
}

static void *shutdown_thread_main(void *arg) {
    struct thread_pool *pool = arg;
    thread_pool_shutdown(pool);
    return NULL;
}

static void test_backpressure_shutdown_wakeup(void) {
    int fd_before = count_open_fds();
    int read_fds[4];
    for (size_t i = 0; i < 4; ++i) {
        int pipefd[2];
        make_tagged_pipe(pipefd, i);
        read_fds[i] = pipefd[0];
    }

    struct gate_ctx ctx = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .gate_open = false
    };

    struct thread_pool pool;
    assert(thread_pool_init(&pool, 1, 2, NULL, gated_task, &ctx) == 0);

    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    assert(thread_pool_submit(&pool, read_fds[0], &ts) == 0);

    pthread_mutex_lock(&ctx.lock);
    while (ctx.started_count < 1) {
        pthread_cond_wait(&ctx.cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    assert(thread_pool_submit(&pool, read_fds[1], &ts) == 0);
    assert(thread_pool_submit(&pool, read_fds[2], &ts) == 0);

    struct blocked_submit_args sub = {
        .pool = &pool,
        .fd = read_fds[3],
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .done = false
    };
    pthread_t submit_thread;
    assert(pthread_create(&submit_thread, NULL, blocked_submit_main, &sub) == 0);

    sleep_ms(30);
    pthread_mutex_lock(&sub.lock);
    assert(sub.done == false);
    pthread_mutex_unlock(&sub.lock);

    pthread_t shutdown_thread;
    assert(pthread_create(&shutdown_thread, NULL, shutdown_thread_main, &pool) == 0);

    /* submit_thread wakes immediately from shutdown broadcast before gate opens. */
    assert(pthread_join(submit_thread, NULL) == 0);
    assert(sub.rc == -1);
    assert(sub.err == ECANCELED);
    assert(sub.elapsed_ms <= 250);
    assert(fcntl(read_fds[3], F_GETFD) != -1);
    assert(close(read_fds[3]) == 0);

    /* Open gate so active worker exits and shutdown_thread completes pthread_join. */
    pthread_mutex_lock(&ctx.lock);
    ctx.gate_open = true;
    pthread_cond_broadcast(&ctx.cond);
    pthread_mutex_unlock(&ctx.lock);

    assert(pthread_join(shutdown_thread, NULL) == 0);

    assert(ctx.finished_count == 1);
    assert(ctx.seen[0] == 1);
    assert(ctx.seen[1] == 0 && ctx.seen[2] == 0);
    for (size_t i = 0; i < 3; ++i) {
        assert_fd_closed(read_fds[i]);
    }

    pthread_mutex_destroy(&sub.lock);
    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.lock);
    assert(count_open_fds() == fd_before);
}

/* --- 4. Shutdown drains unstarted queued FDs & joins active worker --- */

static void *delayed_unblock_main(void *arg) {
    struct gate_ctx *ctx = arg;
    sleep_ms(40);
    pthread_mutex_lock(&ctx->lock);
    ctx->gate_open = true;
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static void test_shutdown_drains_unstarted_fds_and_joins_active_worker(void) {
    int fd_before = count_open_fds();
    int read_fds[5];
    for (size_t i = 0; i < 5; ++i) {
        int pipefd[2];
        make_tagged_pipe(pipefd, i);
        read_fds[i] = pipefd[0];
    }

    struct gate_ctx ctx = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .gate_open = false
    };

    struct thread_pool pool;
    assert(thread_pool_init(&pool, 1, 4, NULL, gated_task, &ctx) == 0);

    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    assert(thread_pool_submit(&pool, read_fds[0], &ts) == 0);

    pthread_mutex_lock(&ctx.lock);
    while (ctx.started_count < 1) {
        pthread_cond_wait(&ctx.cond, &ctx.lock);
    }
    pthread_mutex_unlock(&ctx.lock);

    for (size_t i = 1; i < 5; ++i) {
        assert(thread_pool_submit(&pool, read_fds[i], &ts) == 0);
    }

    pthread_t unblocker;
    assert(pthread_create(&unblocker, NULL, delayed_unblock_main, &ctx) == 0);

    thread_pool_shutdown(&pool);
    assert(pthread_join(unblocker, NULL) == 0);

    assert(ctx.finished_count == 1);
    assert(ctx.seen[0] == 1);
    for (size_t i = 1; i < 5; ++i) {
        assert(ctx.seen[i] == 0);
    }
    for (size_t i = 0; i < 5; ++i) {
        assert_fd_closed(read_fds[i]);
    }

    pthread_cond_destroy(&ctx.cond);
    pthread_mutex_destroy(&ctx.lock);
    assert(count_open_fds() == fd_before);
}

int main(void) {
    test_invalid_args_and_immediate_shutdown();
    test_exact_once_and_fd_ownership();
    test_backpressure_unblock_on_worker_finish();
    test_backpressure_running_flag_escape();
    test_backpressure_shutdown_wakeup();
    test_shutdown_drains_unstarted_fds_and_joins_active_worker();
    printf("[PASS] test_thread_pool\n");
    return 0;
}
