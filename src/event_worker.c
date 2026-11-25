#define _GNU_SOURCE
#include "event_internal.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

int64_t monotonic_milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void notify_event_fd(int fd) {
    uint64_t one = 1;
    ssize_t count;
    do { count = write(fd, &one, sizeof(one)); } while (count < 0 && errno == EINTR);
    /* EAGAIN이면 이미 읽을 알림이 쌓여 있으므로 추가 알림이 필요 없다. */
}

int initialize_event_worker(struct event_worker *worker, struct event_runtime *runtime) {
    worker->runtime = runtime;
    worker->epoll_fd = worker->wake_fd = -1;
    atomic_init(&worker->reserved, 0);
    atomic_init(&worker->stopping, false);
    int error = pthread_mutex_init(&worker->inbox_mutex, NULL);
    if (error) { errno = error; return -1; }
    worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker->epoll_fd < 0) goto fail;
    worker->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (worker->wake_fd < 0) goto fail;
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

void destroy_event_worker(struct event_worker *worker) {
    close(worker->wake_fd);
    close(worker->epoll_fd);
    pthread_mutex_destroy(&worker->inbox_mutex);
}

bool assign_client_to_worker(struct event_worker *worker, struct accepted_client client) {
    bool assigned = false;
    pthread_mutex_lock(&worker->inbox_mutex);
    if (!atomic_load(&worker->stopping) && worker->accept_count < ACCEPT_QUEUE_CAPACITY &&
        atomic_load(&worker->reserved) < WORKER_CONNECTION_LIMIT) {
        size_t tail = (worker->accept_head + worker->accept_count) % ACCEPT_QUEUE_CAPACITY;
        worker->accepted[tail] = client;
        ++worker->accept_count;
        atomic_fetch_add(&worker->reserved, 1);
        assigned = true;
    }
    pthread_mutex_unlock(&worker->inbox_mutex);
    if (assigned) notify_event_fd(worker->wake_fd);
    return assigned;
}

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
static void swap_timers(struct event_worker *worker, size_t a, size_t b) {
    struct event_connection *temporary = worker->timers[a];
    worker->timers[a] = worker->timers[b];
    worker->timers[b] = temporary;
    worker->timers[a]->heap_index = a;
    worker->timers[b]->heap_index = b;
}

static size_t move_timer_up(struct event_worker *worker, size_t index) {
    while (index && worker->timers[index]->timer_at < worker->timers[(index - 1) / 2]->timer_at) {
        size_t parent = (index - 1) / 2;
        swap_timers(worker, index, parent);
        index = parent;
    }
    return index;
}

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

void forget_connection_timer(struct event_connection *connection) {
    if (connection->heap_index == SIZE_MAX) return;
    struct event_worker *worker = connection->worker;
    size_t index = connection->heap_index;
    connection->heap_index = SIZE_MAX;
    --worker->timer_count;
    if (index == worker->timer_count) return;
    worker->timers[index] = worker->timers[worker->timer_count];
    worker->timers[index]->heap_index = index;
    index = move_timer_up(worker, index);
    move_timer_down(worker, index);
}

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

static void collect_retired_connections(struct event_worker *worker) {
    struct event_connection **link = &worker->retired;
    while (*link) {
        struct event_connection *connection = *link;
        if (connection->ready_queued) { link = &connection->retired_next; continue; }
        *link = connection->retired_next;
        release_event_connection(connection);
    }
}

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
    for (size_t i = 0; i < count; ++i) {
        if (atomic_load(&worker->stopping)) {
            close(clients[i].fd);
            atomic_fetch_sub(&worker->reserved, 1);
        } else if (!create_event_connection(worker, clients[i])) {
            close(clients[i].fd);
            atomic_fetch_sub(&worker->reserved, 1);
        }
    }
    while (completed) {
        struct local_job *next = completed->next;
        complete_local_job(completed->connection);
        completed = next;
    }
    submit_waiting_local_jobs(worker);
}

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

void *run_worker_event_loop(void *argument) {
    struct event_worker *worker = argument;
    bool epoll_failed = false;
    struct epoll_event events[EVENT_BATCH_SIZE];
    for (;;) {
        receive_worker_inbox(worker);
        if (atomic_load(&worker->stopping)) begin_worker_shutdown(worker);

        /* 사용자 메모리에 이미 남은 데이터도 처리한다. budget을 넘기면 꼬리에 다시 넣는다. */
        for (size_t i = 0; i < EVENT_BATCH_SIZE && worker->ready_head; ++i) {
            struct event_connection *connection = worker->ready_head;
            worker->ready_head = connection->ready_next;
            if (!worker->ready_head) worker->ready_tail = NULL;
            connection->ready_queued = false;
            if (!connection->retired && !connection->closing) advance_connection(connection);
        }
        collect_retired_connections(worker);
        if (worker->shutdown_started && atomic_load(&worker->reserved) == 0 &&
            !worker->ready_head) break;

        int64_t now = monotonic_milliseconds();
        if (now < 0) epoll_failed = true;
        while (worker->timer_count && worker->timers[0]->timer_at <= now) {
            struct event_connection *connection = worker->timers[0];
            forget_connection_timer(connection);
            expire_connection(connection);
        }
        int timeout = -1;
        if (worker->ready_head) timeout = 0;
        else if (worker->timer_count) {
            int64_t remaining = worker->timers[0]->timer_at - monotonic_milliseconds();
            timeout = remaining <= 0 ? 0 : remaining > INT_MAX ? INT_MAX : (int)remaining;
        }
        if (epoll_failed) {
            /* 작업자가 고장 나도 보조 작업이 참조하는 연결을 먼저 free하지 않는다. */
            atomic_store(&worker->runtime->failed, true);
            notify_event_fd(worker->runtime->failure_fd);
            atomic_store(&worker->stopping, true);
            begin_worker_shutdown(worker);
            if (atomic_load(&worker->reserved) == 0) continue;
            struct pollfd wake = {.fd = worker->wake_fd, .events = POLLIN};
            poll(&wake, 1, 100);
            continue;
        }
        int count = epoll_wait(worker->epoll_fd, events, EVENT_BATCH_SIZE, timeout);
        if (count < 0) {
            if (errno != EINTR) epoll_failed = true;
            continue;
        }
        for (int i = 0; i < count; ++i) {
            if (!events[i].data.ptr) receive_worker_inbox(worker);
            else handle_connection_event(events[i].data.ptr, events[i].events);
        }
        /* 모든 이벤트 포인터의 사용이 끝난 뒤에만 retired를 해제한다. */
        collect_retired_connections(worker);
    }
    collect_retired_connections(worker);
    return NULL;
}
