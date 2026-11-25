#define _GNU_SOURCE
#include "event_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

/* 수락 분배와 조회 A/B 분배는 별개다. 여기서는 여유가 많은 worker를 먼저 고른다. */
static bool hand_client_to_worker(struct event_runtime *runtime,
                                   struct accepted_client client, unsigned int *cursor) {
    unsigned int selected = *cursor;
    size_t fewest = SIZE_MAX;
    for (unsigned int i = 0; i < runtime->worker_count; ++i) {
        unsigned int candidate = (*cursor + i) % runtime->worker_count;
        size_t count = atomic_load(&runtime->workers[candidate].reserved);
        if (count < fewest) { fewest = count; selected = candidate; }
    }
    for (unsigned int i = 0; i < runtime->worker_count; ++i) {
        unsigned int candidate = (selected + i) % runtime->worker_count;
        if (assign_client_to_worker(&runtime->workers[candidate], client)) {
            *cursor = (candidate + 1) % runtime->worker_count;
            return true;
        }
    }
    return false;
}

static int accept_connections(int listener_fd, int signal_fd, struct event_runtime *runtime) {
    unsigned int cursor = 0;
    int64_t accept_retry_at = 0;
    int spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int result = 0;
    for (;;) {
        int64_t now = monotonic_milliseconds();
        if (now < 0) { result = -1; break; }
        int timeout = accept_retry_at > now ? (int)(accept_retry_at - now) : -1;
        struct pollfd interests[] = {
            {.fd = timeout < 0 ? listener_fd : -1, .events = POLLIN},
            {.fd = signal_fd, .events = POLLIN},
            {.fd = runtime->failure_fd, .events = POLLIN}
        };
        int ready = poll(interests, 3, timeout);
        if (ready < 0) { if (errno == EINTR) continue; result = -1; break; }
        if (interests[1].revents) break;
        if (interests[2].revents || atomic_load(&runtime->failed)) { result = -1; break; }
        if (!interests[0].revents) continue;
        /* 무한 accept 루프로 종료 신호가 굶지 않도록 한 번에 batch만 수락한다. */
        for (unsigned int i = 0; i < EVENT_BATCH_SIZE; ++i) {
            int fd = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR || errno == ECONNABORTED) continue;
                if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
                    /* FD 부족 시 한 연결을 버리고 잠시 수락을 멈춘다. 준비된 요청은 계속 처리한다. */
                    if (spare_fd >= 0) close(spare_fd);
                    int rejected = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (rejected >= 0) close(rejected);
                    spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                    accept_retry_at = monotonic_milliseconds() + 100;
                    break;
                }
                perror("accept4");
                result = -1;
                goto done;
            }
            struct accepted_client client = {fd, monotonic_milliseconds()};
            /* 이 함수가 true인 순간부터 해당 worker가 fd를 소유한다. */
            if (client.accepted_at < 0 || !hand_client_to_worker(runtime, client, &cursor))
                close(fd);
        }
    }
done:
    if (spare_fd >= 0) close(spare_fd);
    return result;
}

int run_event_server(int listener_fd, const struct proxy_config *config,
                     unsigned int worker_count) {
    if (!worker_count || worker_count > EVENT_MAX_WORKERS) { errno = EINVAL; return -1; }
    struct event_runtime runtime = {.config = config, .worker_count = worker_count, .failure_fd = -1};
    atomic_init(&runtime.failed, false);
    runtime.workers = calloc(worker_count, sizeof(*runtime.workers));
    if (!runtime.workers) return -1;
    sigset_t shutdown_signals, previous_mask;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &shutdown_signals, &previous_mask);
    if (error) { free(runtime.workers); errno = error; return -1; }
    int signal_fd = signalfd(-1, &shutdown_signals, SFD_NONBLOCK | SFD_CLOEXEC);
    unsigned int initialized = 0;
    bool selection_initialized = false, pool_started = false;
    int result = -1;
    if (signal_fd < 0) goto cleanup;
    int flags = fcntl(listener_fd, F_GETFL);
    if (flags < 0 || fcntl(listener_fd, F_SETFL, flags | O_NONBLOCK) < 0) goto cleanup;
    error = pthread_mutex_init(&runtime.selection_mutex, NULL);
    if (error) { errno = error; goto cleanup; }
    selection_initialized = true;
    runtime.failure_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (runtime.failure_fd < 0) goto cleanup;
    for (; initialized < worker_count; ++initialized)
        if (initialize_event_worker(&runtime.workers[initialized], &runtime) < 0) goto cleanup;
    if (start_local_work_pool(&runtime.local_pool) < 0) goto cleanup;
    pool_started = true;
    for (unsigned int i = 0; i < worker_count; ++i) {
        error = pthread_create(&runtime.workers[i].thread, NULL, run_worker_event_loop,
                               &runtime.workers[i]);
        if (error) { errno = error; goto cleanup; }
        runtime.workers[i].started = true;
    }
    result = accept_connections(listener_fd, signal_fd, &runtime);
cleanup:
    {
        int saved = errno;
        /* 생성 순서의 역순. 보조 풀이 먼저 사라지면 worker의 cleanup이 끝날 수 없다. */
        for (unsigned int i = 0; i < initialized; ++i) {
            atomic_store(&runtime.workers[i].stopping, true);
            notify_event_fd(runtime.workers[i].wake_fd);
        }
        for (unsigned int i = 0; i < initialized; ++i)
            if (runtime.workers[i].started) pthread_join(runtime.workers[i].thread, NULL);
        if (pool_started) stop_local_work_pool(&runtime.local_pool);
        for (unsigned int i = 0; i < initialized; ++i) destroy_event_worker(&runtime.workers[i]);
        if (runtime.failure_fd >= 0) close(runtime.failure_fd);
        if (selection_initialized) pthread_mutex_destroy(&runtime.selection_mutex);
        if (signal_fd >= 0) {
            /* pending 신호를 소비한 뒤 원래 mask를 복구한다. */
            struct signalfd_siginfo info;
            while (read(signal_fd, &info, sizeof(info)) > 0) {}
            close(signal_fd);
        }
        pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
        free(runtime.workers);
        errno = saved;
    }
    return result;
}
