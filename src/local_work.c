#define _POSIX_C_SOURCE 200809L
#include "event_internal.h"

#include <errno.h>
#include <stdlib.h>

/* owner의 submit_local_job → 보조 스레드의 run_local_work_loop → 완료 큐/eventfd
 * → owner의 receive_worker_inbox/complete_local_job → 네트워크 송신 재개.
 * 큐 전달을 경계로 provider 접근권을 넘기며, 작업 중 연결 메모리를 해제하지 않는다. */

/* 할 일이 생길 때까지 조건변수로 기다리고 한 작업을 꺼낸다. 반환 시 mutex는 풀려 있다.
 * NULL이면 종료 표시가 있고 남은 작업도 없다는 뜻이다. */
static struct local_job *take_next_local_job(struct local_work_pool *pool) {
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
    return job;
}

/* 보조 큐에 빈자리가 났으므로 각 네트워크 작업자의 제출 대기 목록을 재시도하게 한다. */
static void notify_local_queue_space(struct event_runtime *runtime) {
    for (unsigned int i = 0; i < runtime->worker_count; ++i)
        notify_event_fd(runtime->workers[i].wake_fd);
}

/* 준비/읽기는 취소할 수 있지만 cleanup은 항상 실행한다. 파일/CGI의 대기 중에는 큐 lock을 잡지 않는다.
 * owner는 완료 결과를 받을 때까지 이 response/provider를 사용하거나 연결을 해제하지 않는다. */
static void execute_local_job(struct local_job *job) {
    struct event_connection *connection = job->connection;
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
        connection->response.body_provider.cleanup_func(connection->response.body_provider.state);
    job->error = errno;
}

/* 결과를 owner의 완료 큐에 공개한다. unlock 뒤에는 job이 곧 재사용/해제될 수 있어 만지지 않는다. */
static void publish_local_job_result(struct local_job *job) {
    struct event_worker *owner = job->connection->worker;
    pthread_mutex_lock(&owner->inbox_mutex);
    job->next = NULL;
    if (owner->completed_tail) owner->completed_tail->next = job;
    else owner->completed_head = job;
    owner->completed_tail = job;
    pthread_mutex_unlock(&owner->inbox_mutex);
    notify_event_fd(owner->wake_fd);
}

/* 보조 스레드의 전체 순서: 작업 수령 → 빈자리 알림 → 파일/CGI 처리 → owner에 결과 전달. */
static void *run_local_work_loop(void *argument) {
    struct local_work_pool *pool = argument;
    for (;;) {
        struct local_job *job = take_next_local_job(pool);
        if (!job) return NULL;
        notify_local_queue_space(job->connection->worker->runtime);
        execute_local_job(job);
        publish_local_job_result(job);
    }
}

/* [메인] 파일/CGI용 고정 스레드 2개를 시작한다. 일부 생성에 실패하면 이미 만든 스레드도 join한다. */
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

/* [메인] 네트워크 worker join 후 호출한다. 잠든 보조 스레드를 모두 깨우고 큐 처리/종료를 기다린다. */
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

/* 상한 있는 보조 풀 큐에 대기 없이 넣는다. 가득 차면 false이며 worker가 직접 대기 목록에 보관한다. */
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

/* [소유 네트워크 worker] 연결에 내장된 job 하나를 보조 풀로 넘긴다.
 * 반환 true=풀 큐 등록, false=owner의 대기 목록 보관이며 둘 다 local_job_inflight는 유지한다. */
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

/* 보조 큐에 빈자리가 생겼다는 알림 뒤 대기 중 작업을 다시 넣어 본다.
 * 연결 종료 시 준비/읽기는 실행 없이 완료 처리하지만 cleanup은 반드시 보조 풀로 전달한다. */
void submit_waiting_local_jobs(struct event_worker *worker) {
    while (worker->waiting_head) {
        struct local_job *job = worker->waiting_head;
        /* 등록 성공 시 job->next가 풀 큐 링크로 바뀌므로 owner 목록의 다음 주소를 먼저 저장한다. */
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
