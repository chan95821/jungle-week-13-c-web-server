#define _GNU_SOURCE
#include "event_internal.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* 읽는 순서: run_worker_event_loop → receive_worker_inbox → create_event_connection
 * → 실행 대기 큐의 advance_connection. 이후 소켓 이벤트는 handle_connection_event로 재개한다.
 * 메인이 쓰는 인수 큐와 owner만 쓰는 실행 큐, 배치 뒤 해제 목록을 구분해서 읽으면 된다. */

/* 시스템 시각 변경에 흔들리지 않는 CLOCK_MONOTONIC을 밀리초로 바꾼다.
 * 연결별 절대 기한과 epoll_wait의 남은 대기 시간을 같은 단위로 계산하기 위해 쓴다. */
int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* [메인/보조 스레드도 호출] 커널의 eventfd 카운터에 1을 써서 담당 작업자를 깨운다.
 * 실제 연결/결과는 공유 큐에 있다. 이 값에 FD나 HTTP 데이터를 담는 것은 아니다. */
void notify_event_fd(int fd) {
    uint64_t one = 1;
    ssize_t count;
    do { count = write(fd, &one, sizeof(one)); } while (count < 0 && errno == EINTR);
    /* EAGAIN이면 이미 읽을 알림이 쌓여 있으므로 추가 알림이 필요 없다. */
}

/* [메인에서 호출] worker 정보와 전용 epoll/알림 FD를 준비한다. 스레드 생성은 아직 하지 않는다.
 * 실패하면 여기서 만든 자원만 회수하고 -1을 반환한다. */
int initialize_event_worker(struct event_worker *worker, struct event_runtime *runtime) {
    worker->runtime = runtime;
    worker->epoll_fd = worker->wake_fd = -1;
    atomic_init(&worker->reserved, 0);
    atomic_init(&worker->stopping, false);
    int error = pthread_mutex_init(&worker->inbox_mutex, NULL);
    if (error) { errno = error; return -1; }
    /* 커널이 관리하는 epoll 인스턴스 하나와 그 worker를 깨울 eventfd 하나를 만든다. */
    worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker->epoll_fd < 0) goto fail;
    worker->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (worker->wake_fd < 0) goto fail;
    /* data.ptr=NULL은 '큐를 확인하라'는 제어 이벤트 표시다.
     * 일반 소켓 이벤트에는 나중에 event_endpoint 주소를 넣어 연결을 찾아간다. */
    struct epoll_event wake = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->wake_fd, &wake) < 0) goto fail;
    return 0;
fail:
    {
        int saved = errno;
        if (worker->wake_fd >= 0) close(worker->wake_fd);
        if (worker->epoll_fd >= 0) close(worker->epoll_fd);
        pthread_mutex_destroy(&worker->inbox_mutex);
        errno = saved;
        return -1;
    }
}

/* [메인에서 호출] join이 끝난 worker의 epoll/알림 FD와 큐 mutex를 해제한다. */
void destroy_event_worker(struct event_worker *worker) {
    close(worker->wake_fd);
    close(worker->epoll_fd);
    pthread_mutex_destroy(&worker->inbox_mutex);
}

/* [메인에서 호출] 선택된 worker의 수락 큐에 client FD와 accept 시각을 넣는다.
 * true이면 소유권 전달 완료, false이면 큐/연결 상한 또는 종료 상태로 등록하지 못한 것이다. */
bool assign_client_to_worker(struct event_worker *worker, struct accepted_client client) {
    bool assigned = false;
    /* 메인은 넣고 worker는 꺼내므로 큐 배열/위치/개수는 같은 mutex로 보호한다. */
    pthread_mutex_lock(&worker->inbox_mutex);
    if (!atomic_load(&worker->stopping) && worker->accept_count < ACCEPT_QUEUE_CAPACITY &&
        atomic_load(&worker->reserved) < WORKER_CONNECTION_LIMIT) {
        /* 원형 큐의 다음 빈칸에 저장하고, 아직 큐에 있는 연결도 reserved에 포함한다. */
        size_t tail = (worker->accept_head + worker->accept_count) % ACCEPT_QUEUE_CAPACITY;
        worker->accepted[tail] = client;
        ++worker->accept_count;
        atomic_fetch_add(&worker->reserved, 1);
        assigned = true;
    }
    pthread_mutex_unlock(&worker->inbox_mutex);
    /* 큐 공개가 끝난 뒤 깨운다. 이미 실행 중이면 다음 inbox 확인 때 새 연결을 가져간다. */
    if (assigned) notify_event_fd(worker->wake_fd);
    return assigned;
}

/* [소유 worker 전용] 지금 이어 처리할 수 있는 연결을 실행 대기 큐에 한 번만 넣는다.
 * 새 연결/보조 결과/메모리에 남은 바이트는 새 소켓 이벤트 없이도 진행할 수 있다. */
void schedule_connection(struct event_connection *connection) {
    if (connection->ready_queued || connection->retired) return;
    struct event_worker *worker = connection->worker;
    connection->ready_queued = true;
    connection->ready_next = NULL;
    if (worker->ready_tail) worker->ready_tail->ready_next = connection;
    else worker->ready_head = connection;
    worker->ready_tail = connection;
}

/* 만료 시각의 최소 힙. 매 이벤트마다 모든 연결을 훑지 않고 최솟값만 기다린다. */
/* 힙의 두 항목을 바꿀 때 연결 안에 저장한 위치도 함께 갱신한다. */
static void swap_timers(struct event_worker *worker, size_t a, size_t b) {
    struct event_connection *temporary = worker->timers[a];
    worker->timers[a] = worker->timers[b];
    worker->timers[b] = temporary;
    worker->timers[a]->heap_index = a;
    worker->timers[b]->heap_index = b;
}

/* 기한이 앞당겨진 항목을 부모와 비교해 올린다. 반환값은 이동이 끝난 위치다. */
static size_t move_timer_up(struct event_worker *worker, size_t index) {
    while (index && worker->timers[index]->timer_at < worker->timers[(index - 1) / 2]->timer_at) {
        size_t parent = (index - 1) / 2;
        swap_timers(worker, index, parent);
        index = parent;
    }
    return index;
}

/* 두 자식 중 더 빠른 기한 쪽과 교환하며 내려 최소 힙 순서를 복구한다. */
static void move_timer_down(struct event_worker *worker, size_t index) {
    for (;;) {
        size_t child = index * 2 + 1;
        if (child >= worker->timer_count) return;
        if (child + 1 < worker->timer_count &&
            worker->timers[child + 1]->timer_at < worker->timers[child]->timer_at) ++child;
        if (worker->timers[index]->timer_at <= worker->timers[child]->timer_at) return;
        swap_timers(worker, index, child);
        index = child;
    }
}

/* 연결 종료/대기 종류 변경 시 타이머 힙에서 제거한다. SIZE_MAX는 등록되지 않았다는 뜻이다. */
void forget_connection_timer(struct event_connection *connection) {
    if (connection->heap_index == SIZE_MAX) return;
    struct event_worker *worker = connection->worker;
    size_t index = connection->heap_index;
    connection->heap_index = SIZE_MAX;
    --worker->timer_count;
    if (index == worker->timer_count) return;
    /* 빠진 자리에 마지막 항목을 옮긴 뒤 위/아래 정렬로 빈자리를 메운다. */
    worker->timers[index] = worker->timers[worker->timer_count];
    worker->timers[index]->heap_index = index;
    index = move_timer_up(worker, index);
    move_timer_down(worker, index);
}

/* 현재 phase에 적용할 가장 빠른 기한을 힙에 등록하거나 갱신한다.
 * 힙과 연결 상태는 소유 worker만 바꾸므로 타이머 전용 mutex/스레드는 필요 없다. */
void update_connection_timer(struct event_connection *connection) {
    int64_t deadline = connection_next_deadline(connection);
    if (deadline < 0) { forget_connection_timer(connection); return; }
    struct event_worker *worker = connection->worker;
    connection->timer_at = deadline;
    if (connection->heap_index == SIZE_MAX) {
        connection->heap_index = worker->timer_count;
        worker->timers[worker->timer_count++] = connection;
    }
    size_t index = move_timer_up(worker, connection->heap_index);
    move_timer_down(worker, index);
}

/* FD/보조 정리를 마친 연결을 활성 목록에서 빼고 '나중에 free할 목록'으로 옮긴다.
 * 여기서 reserved를 줄여 메인이 새 연결 배정에 이 여유를 반영할 수 있게 한다. */
void retire_connection(struct event_connection *connection) {
    if (connection->retired) return;
    struct event_worker *worker = connection->worker;
    forget_connection_timer(connection);
    if (connection->previous) connection->previous->next = connection->next;
    else worker->connections = connection->next;
    if (connection->next) connection->next->previous = connection->previous;
    connection->phase = CONNECTION_CLOSED;
    connection->retired = true;
    atomic_fetch_sub(&worker->reserved, 1);
    /* 같은 epoll_wait 결과 배열에 client/backend 이벤트가 함께 있을 수 있다.
     * 지금 free하면 뒤의 이벤트가 해제된 포인터를 읽는다. 배치 끝까지 메모리를 보류한다. */
    connection->retired_next = worker->retired;
    worker->retired = connection;
}

/* 현재 epoll 이벤트 배치가 더 이상 참조하지 않는 연결 메모리를 해제한다.
 * 실행 대기 큐에도 포인터가 남아 있으면 그 큐에서 빠질 때까지 한 번 더 보류한다. */
static void collect_retired_connections(struct event_worker *worker) {
    struct event_connection **link = &worker->retired;
    while (*link) {
        struct event_connection *connection = *link;
        if (connection->ready_queued) { link = &connection->retired_next; continue; }
        *link = connection->retired_next;
        release_event_connection(connection);
    }
}

/* 큐에서 인수한 FD를 연결 상태로 만든다. 종료/할당 실패면 소유 FD와 예약 수를 함께 회수한다. */
static void create_queued_connections(struct event_worker *worker,
                                       const struct accepted_client *clients, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (atomic_load(&worker->stopping) || !create_event_connection(worker, clients[i])) {
            close(clients[i].fd);
            atomic_fetch_sub(&worker->reserved, 1);
        }
    }
}

/* 결과 반영 중 같은 job이 재사용될 수 있으므로 다음 링크를 먼저 저장한다. */
static void apply_completed_local_jobs(struct local_job *completed) {
    while (completed) {
        struct local_job *next = completed->next;
        complete_local_job(completed->connection);
        completed = next;
    }
}

/* 공유 큐의 내용을 lock 안에서 분리하고, 실제 연결/결과 처리는 unlock 뒤에 한다.
 * eventfd는 '확인할 일이 있다'는 알림일 뿐이므로 큐 항목 수와 같을 필요가 없다. */
static void receive_worker_inbox(struct event_worker *worker) {
    uint64_t value;
    while (read(worker->wake_fd, &value, sizeof(value)) > 0) {}
    struct accepted_client clients[ACCEPT_QUEUE_CAPACITY];
    pthread_mutex_lock(&worker->inbox_mutex);
    size_t count = worker->accept_count;
    for (size_t i = 0; i < count; ++i)
        clients[i] = worker->accepted[(worker->accept_head + i) % ACCEPT_QUEUE_CAPACITY];
    worker->accept_count = 0;
    worker->accept_head = 0;
    struct local_job *completed = worker->completed_head;
    worker->completed_head = worker->completed_tail = NULL;
    pthread_mutex_unlock(&worker->inbox_mutex);

    create_queued_connections(worker, clients, count);
    apply_completed_local_jobs(completed);
    submit_waiting_local_jobs(worker);
}

/* 종료 요청은 한 번만 반영한다. 소유 연결의 FD를 닫고, 진행 중 보조 작업의 결과/정리는 기다린다. */
static void begin_worker_shutdown(struct event_worker *worker) {
    if (worker->shutdown_started) return;
    worker->shutdown_started = true;
    for (struct event_connection *connection = worker->connections; connection;) {
        struct event_connection *next = connection->next;
        close_event_connection(connection);
        connection = next;
    }
    submit_waiting_local_jobs(worker);
}

/* 메모리에 이미 준비된 연결을 공통 budget만큼 진행한다. 새 epoll 이벤트 없이도 이어갈 수 있다. */
static void process_ready_connections(struct event_worker *worker) {
    for (size_t i = 0; i < EVENT_BATCH_SIZE && worker->ready_head; ++i) {
        struct event_connection *connection = worker->ready_head;
        worker->ready_head = connection->ready_next;
        if (!worker->ready_head) worker->ready_tail = NULL;
        connection->ready_queued = false;
        if (!connection->retired && !connection->closing) advance_connection(connection);
    }
}

/* 가장 빠른 만료부터 처리한다. 시계를 읽을 수 없으면 false로 작업자 오류 정리를 요청한다. */
static bool expire_due_connections(struct event_worker *worker) {
    int64_t now = monotonic_milliseconds();
    if (now < 0) return false;
    while (worker->timer_count && worker->timers[0]->timer_at <= now) {
        struct event_connection *connection = worker->timers[0];
        forget_connection_timer(connection);
        expire_connection(connection);
    }
    return true;
}

/* 실행할 일이 있으면 0, 기한이 있으면 남은 밀리초, 둘 다 없으면 무기한 대기 -1이다. */
static int next_epoll_timeout(const struct event_worker *worker) {
    if (worker->ready_head) return 0;
    if (!worker->timer_count) return -1;
    int64_t remaining = worker->timers[0]->timer_at - monotonic_milliseconds();
    return remaining <= 0 ? 0 : remaining > INT_MAX ? INT_MAX : (int)remaining;
}

/* 종료 표시만으로 나가지 않고, 연결/보조 정리와 실행 대기 포인터가 모두 소진됐는지 확인한다. */
static bool worker_shutdown_finished(const struct event_worker *worker) {
    return worker->shutdown_started && atomic_load(&worker->reserved) == 0 && !worker->ready_head;
}

/* epoll 실패도 메모리 수명은 지켜야 한다. 메인에 알린 뒤 보조 결과를 eventfd로 회수하며 정리한다. */
static void wait_for_failed_worker_cleanup(struct event_worker *worker) {
    atomic_store(&worker->runtime->failed, true);
    notify_event_fd(worker->runtime->failure_fd);
    atomic_store(&worker->stopping, true);
    begin_worker_shutdown(worker);
    if (atomic_load(&worker->reserved) == 0) return;
    struct pollfd wake = {.fd = worker->wake_fd, .events = POLLIN};
    poll(&wake, 1, 100);
}

/* NULL은 큐 확인용 eventfd, 나머지는 해당 연결의 client/backend endpoint다. */
static void process_worker_events(struct event_worker *worker,
                                   const struct epoll_event *events, int count) {
    for (int i = 0; i < count; ++i) {
        if (!events[i].data.ptr) receive_worker_inbox(worker);
        else handle_connection_event(events[i].data.ptr, events[i].events);
    }
}

/* pthread_create가 시작하는 작업자 본체. 한 바퀴의 순서를 읽으면 전체 실행 흐름이 보인다.
 * 메모리 해제는 ready 큐 또는 epoll 배치의 포인터 사용이 끝난 지점에서만 수행한다. */
void *run_worker_event_loop(void *argument) {
    struct event_worker *worker = argument;
    struct epoll_event events[EVENT_BATCH_SIZE];
    bool epoll_failed = false;
    for (;;) {
        receive_worker_inbox(worker);
        if (atomic_load(&worker->stopping)) begin_worker_shutdown(worker);
        process_ready_connections(worker);
        collect_retired_connections(worker);
        if (worker_shutdown_finished(worker)) break;

        if (!expire_due_connections(worker)) epoll_failed = true;
        int timeout = next_epoll_timeout(worker);
        if (epoll_failed) {
            wait_for_failed_worker_cleanup(worker);
            continue;
        }
        int count = epoll_wait(worker->epoll_fd, events, EVENT_BATCH_SIZE, timeout);
        if (count < 0) {
            if (errno != EINTR) epoll_failed = true;
            continue;
        }
        process_worker_events(worker, events, count);
        collect_retired_connections(worker);
    }
    collect_retired_connections(worker);
    return NULL;
}
