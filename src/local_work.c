#define _POSIX_C_SOURCE 200809L
#include "event_internal.h"

#include <errno.h>
#include <stdlib.h>

/* 파일/CGI만 담당한다. 클라이언트/백엔드 socket의 recv/send는 여기서 하지 않는다. */
static void *run_local_work_loop(void *argument) {
    struct local_work_pool *pool = argument;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->head && !pool->stopping)
            pthread_cond_wait(&pool->available, &pool->mutex);
        if (!pool->head && pool->stopping) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        struct local_job *job = pool->head;
        pool->head = job->next;
        if (!pool->head) pool->tail = NULL;
        --pool->queued;
        pthread_mutex_unlock(&pool->mutex);

        struct event_connection *connection = job->connection;
        struct event_worker *owner = connection->worker;
        /* 큐에 빈자리가 생겼다. 다른 작업자의 대기 중인 로컬 작업도 재시도한다. */
        for (unsigned int i = 0; i < owner->runtime->worker_count; ++i)
            notify_event_fd(owner->runtime->workers[i].wake_fd);

        errno = 0;
        job->chunk = (struct http_slice){0};
        if (job->kind != CLEANUP_LOCAL_RESPONSE && atomic_load(&connection->cancel_local_job)) {
            job->result = BODY_ERROR;
            errno = ECANCELED;
        } else if (job->kind == PREPARE_LOCAL_RESPONSE)
            connection->route.local_handler(&connection->request, &connection->response);
        else if (job->kind == READ_LOCAL_BODY)
            job->result = connection->response.body_provider.next_func(
                connection->response.body_provider.state, &job->chunk);
        else if (connection->response.body_provider.cleanup_func)
            connection->response.body_provider.cleanup_func(
                connection->response.body_provider.state);
        job->error = errno;

        /* mutex의 release/acquire가 결과를 owner에게 공개한다. 알림 자체가 lock은 아니다. */
        pthread_mutex_lock(&owner->inbox_mutex);
        job->next = NULL;
        if (owner->completed_tail) owner->completed_tail->next = job;
        else owner->completed_head = job;
        owner->completed_tail = job;
        pthread_mutex_unlock(&owner->inbox_mutex);
        notify_event_fd(owner->wake_fd);
    }
}

int start_local_work_pool(struct local_work_pool *pool) {
    int error = pthread_mutex_init(&pool->mutex, NULL);
    if (error) { errno = error; return -1; }
    error = pthread_cond_init(&pool->available, NULL);
    if (error) { pthread_mutex_destroy(&pool->mutex); errno = error; return -1; }
    for (unsigned int i = 0; i < LOCAL_THREAD_COUNT; ++i) {
        error = pthread_create(&pool->threads[i], NULL, run_local_work_loop, pool);
        if (error) {
            errno = error;
            stop_local_work_pool(pool);
            return -1;
        }
        ++pool->started;
    }
    return 0;
}

void stop_local_work_pool(struct local_work_pool *pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->available);
    pthread_mutex_unlock(&pool->mutex);
    for (unsigned int i = 0; i < pool->started; ++i) pthread_join(pool->threads[i], NULL);
    pthread_cond_destroy(&pool->available);
    pthread_mutex_destroy(&pool->mutex);
    pool->started = 0;
}

static bool try_queue_local_job(struct local_job *job) {
    struct local_work_pool *pool = &job->connection->worker->runtime->local_pool;
    pthread_mutex_lock(&pool->mutex);
    if (pool->queued == LOCAL_JOB_CAPACITY) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    job->next = NULL;
    if (pool->tail) pool->tail->next = job;
    else pool->head = job;
    pool->tail = job;
    ++pool->queued;
    pthread_cond_signal(&pool->available);
    pthread_mutex_unlock(&pool->mutex);
    return true;
}

bool submit_local_job(struct event_connection *connection, enum local_job_kind kind) {
    struct event_worker *worker = connection->worker;
    struct local_job *job = &connection->job;
    *job = (struct local_job){.connection = connection, .kind = kind};
    connection->local_job_inflight = true;
    if (try_queue_local_job(job)) return true;
    /* 풀 큐가 가득 차도 worker는 기다리지 않는다. 연결에 이미 있는 job을 보관한다.
     * 이 대기열 역시 작업자 연결 상한 이내다. 특히 cleanup 작업을 잃어버리지 않는다. */
    job->next = NULL;
    if (worker->waiting_tail) worker->waiting_tail->next = job;
    else worker->waiting_head = job;
    worker->waiting_tail = job;
    return false;
}

void submit_waiting_local_jobs(struct event_worker *worker) {
    while (worker->waiting_head) {
        struct local_job *job = worker->waiting_head;
        struct local_job *next = job->next;
        /* 아직 실행하지 않은 준비/읽기는 종료 시 취소할 수 있다. cleanup은 반드시 실행한다. */
        if (job->connection->closing && job->kind != CLEANUP_LOCAL_RESPONSE) {
            worker->waiting_head = next;
            if (!next) worker->waiting_tail = NULL;
            complete_local_job(job->connection);
            continue;
        }
        if (!try_queue_local_job(job)) break;
        worker->waiting_head = next;
        if (!next) worker->waiting_tail = NULL;
    }
}
