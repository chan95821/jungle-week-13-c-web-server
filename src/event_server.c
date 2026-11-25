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

/* 이 파일의 실행 주체는 메인 스레드다.
 * run_event_server: 고정 스레드 준비 → accept_connections: poll/accept4 반복
 * → hand_client_to_worker: 연결 수 비교 → assign_client_to_worker: 큐/알림 전달.
 * 작업자에게 넘긴 이후의 HTTP 처리는 event_worker.c / event_connection.c에서 이어진다. */

/* 연결 수는 순간 부하의 추정값이다. 같은 최소값이면 cursor부터 탐색해 편중을 줄인다. */
static unsigned int choose_worker_with_fewest_connections(const struct event_runtime *runtime,
                                                          unsigned int cursor) {
    unsigned int selected = cursor;
    size_t fewest = SIZE_MAX;
    for (unsigned int i = 0; i < runtime->worker_count; ++i) {
        unsigned int candidate = (cursor + i) % runtime->worker_count;
        size_t count = atomic_load(&runtime->workers[candidate].reserved);
        if (count < fewest) { fewest = count; selected = candidate; }
    }
    return selected;
}

/* [메인] 여유 작업자를 우선 시도한다. true부터 FD 정리 책임은 작업자에게 넘어간다.
 * 선택 뒤 큐가 가득 찰 수 있으므로 실제 등록에 실패하면 다른 작업자도 순서대로 시도한다. */
static bool hand_client_to_worker(struct event_runtime *runtime,
                                   struct accepted_client client, unsigned int *cursor) {
    unsigned int selected = choose_worker_with_fewest_connections(runtime, *cursor);
    for (unsigned int i = 0; i < runtime->worker_count; ++i) {
        unsigned int candidate = (selected + i) % runtime->worker_count;
        if (assign_client_to_worker(&runtime->workers[candidate], client)) {
            *cursor = (candidate + 1) % runtime->worker_count;
            return true;
        }
    }
    return false;
}

/* 여분 FD를 잠시 반납해 대기 연결 하나를 수락/거절하고 여분을 다시 확보한다. */
static int reject_one_pending_connection(int listener_fd, int spare_fd) {
    if (spare_fd >= 0) close(spare_fd);
    int rejected = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (rejected >= 0) close(rejected);
    return open("/dev/null", O_RDONLY | O_CLOEXEC);
}

// accept4 : 새 클라이언트 FD를 얻는다.
/* [메인의 접속 수락 루프] poll 대기 → accept4 → 작업자 선택/큐 전달을 반복한다.
 * HTTP 헤더/본문을 여기서 읽지 않는다. 정상 종료 0, 실행 오류 -1을 반환한다. */
static int accept_connections(int listener_fd, int signal_fd, struct event_runtime *runtime) {
    unsigned int cursor = 0;
    int64_t accept_retry_at = 0;
    /* FD가 고갈돼도 대기 연결 하나를 수락 후 버릴 수 있도록 여분 FD 하나를 확보한다. */
    int spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int result = 0;
    for (;;) {
        int64_t now = monotonic_milliseconds();
        if (now < 0) { result = -1; break; }
        int timeout = accept_retry_at > now ? (int)(accept_retry_at - now) : -1;
        /* 수락을 잠시 쉬는 구간에도 종료/작업자 오류 알림은 계속 감시한다.
         * 평소 timeout=-1이면 이벤트까지 대기하고, 이미 이벤트가 있으면 즉시 반환한다. */
        struct pollfd interests[] = {
            {.fd = timeout < 0 ? listener_fd : -1, .events = POLLIN},
            {.fd = signal_fd, .events = POLLIN},
            {.fd = runtime->failure_fd, .events = POLLIN}
        };
        /* 새 TCP 연결 또는 제어 알림이 생기면 커널이 대기를 풀어 준다. */
        int ready = poll(interests, 3, timeout);
        if (ready < 0) { if (errno == EINTR) continue; result = -1; break; }
        if (interests[1].revents) break;
        if (interests[2].revents || atomic_load(&runtime->failed)) { result = -1; break; }
        if (!interests[0].revents) continue;
        /* 무한 accept 루프로 종료 신호가 굶지 않도록 한 번에 batch만 수락한다. */
        for (unsigned int i = 0; i < EVENT_BATCH_SIZE; ++i) {
            /* 리스너는 유지하고 새 연결용 FD를 얻는다. NONBLOCK은 I/O 대기를 피하고,
             * CLOEXEC은 동시에 시작되는 CGI 실행 파일에 이 FD가 남는 것을 막는다. */
            int fd = accept4(listener_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                /* 수락할 연결을 모두 꺼냈다. 바깥 poll로 돌아가 다음 알림을 기다린다. */
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR || errno == ECONNABORTED) continue;
                if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
                    /* FD 부족 시 한 연결을 버리고 잠시 수락을 멈춘다. 준비된 요청은 계속 처리한다. */
                    spare_fd = reject_one_pending_connection(listener_fd, spare_fd);
                    accept_retry_at = monotonic_milliseconds() + 100;
                    break;
                }
                perror("accept4");
                result = -1;
                goto done;
            }
            /* accept 시각을 함께 넘겨 큐 대기 시간도 요청 헤더 전체 기한에 포함한다. */
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

/* 모든 worker의 알림 FD를 스레드 시작 전에 준비한다. 성공한 개수만 caller에 남겨 실패 시 정리한다. */
static int initialize_network_workers(struct event_runtime *runtime, unsigned int *initialized) {
    while (*initialized < runtime->worker_count) {
        if (initialize_event_worker(&runtime->workers[*initialized], runtime) < 0) return -1;
        ++*initialized;
    }
    return 0;
}

/* pthread_create로 고정 네트워크 풀을 시작한다. 일부 실패한 경우 started 표시로 join 대상을 구분한다. */
static int start_network_workers(struct event_runtime *runtime) {
    for (unsigned int i = 0; i < runtime->worker_count; ++i) {
        struct event_worker *worker = &runtime->workers[i];
        int error = pthread_create(&worker->thread, NULL, run_worker_event_loop, worker);
        if (error) { errno = error; return -1; }
        worker->started = true;
    }
    return 0;
}

/* 전체에 종료를 알린 뒤 join한다. 이 함수가 끝날 때까지 보조 풀은 살아 있어야 cleanup이 가능하다. */
static void stop_network_workers(struct event_runtime *runtime, unsigned int initialized) {
    for (unsigned int i = 0; i < initialized; ++i) {
        atomic_store(&runtime->workers[i].stopping, true);
        notify_event_fd(runtime->workers[i].wake_fd);
    }
    for (unsigned int i = 0; i < initialized; ++i)
        if (runtime->workers[i].started) pthread_join(runtime->workers[i].thread, NULL);
}

/* [main의 조율 함수] 메모리/알림 FD 준비 → 보조 풀/네트워크 작업자 생성 → 메인 수락.
 * 종료할 때 작업자를 모두 join한 뒤 공유 상태를 해제한다. config/listener는 빌려 쓴다. */
int run_event_server(int listener_fd, const struct proxy_config *config,
                     unsigned int worker_count) {
    if (!worker_count || worker_count > EVENT_MAX_WORKERS) { errno = EINVAL; return -1; }
    struct event_runtime runtime = {.config = config, .worker_count = worker_count, .failure_fd = -1};
    atomic_init(&runtime.failed, false);
    /* calloc은 작업자 정보를 담을 배열만 만든다. 실제 스레드는 아래 pthread_create로 만든다. */
    runtime.workers = calloc(worker_count, sizeof(*runtime.workers));
    if (!runtime.workers) return -1;
    /* 작업자 생성 전에 종료 신호를 차단하면 새 스레드도 같은 mask를 물려받는다.
     * 신호를 무시하는 것이 아니라 메인의 signalfd로 모아 정리 순서를 통제한다. */
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
    /* 리스너도 nonblocking이어야 수락 대기열이 비었을 때 메인이 accept에서 멈추지 않는다. */
    int flags = fcntl(listener_fd, F_GETFL);
    if (flags < 0 || fcntl(listener_fd, F_SETFL, flags | O_NONBLOCK) < 0) goto cleanup;
    /* 전체 작업자가 공유하는 조회 A/B 선택 순번만 이 mutex로 보호한다. */
    error = pthread_mutex_init(&runtime.selection_mutex, NULL);
    if (error) { errno = error; goto cleanup; }
    selection_initialized = true;
    runtime.failure_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (runtime.failure_fd < 0) goto cleanup;
    if (initialize_network_workers(&runtime, &initialized) < 0) goto cleanup;
    /* 파일/CGI 대기용 고정 보조 풀부터 시작한다. 네트워크 recv/send를 맡기는 풀은 아니다. */
    if (start_local_work_pool(&runtime.local_pool) < 0) goto cleanup;
    pool_started = true;
    if (start_network_workers(&runtime) < 0) goto cleanup;
    /* 새 스레드를 만드는 호출이 아니다. 메인 스레드가 이 함수 안에서 계속 수락한다. */
    result = accept_connections(listener_fd, signal_fd, &runtime);
cleanup:
    {
        int saved = errno;
        /* 부분 초기화 실패도 같은 경로로 회수한다. 보조 풀은 네트워크 worker join 이후에 종료한다. */
        stop_network_workers(&runtime, initialized);
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
