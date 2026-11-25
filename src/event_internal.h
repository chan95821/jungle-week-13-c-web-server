#ifndef EVENT_INTERNAL_H
#define EVENT_INTERNAL_H

/* B 모델 내부에서만 공유하는 타입. 연결 데이터의 수정자는 항상 소유 worker다.
 * 보조 작업 중에는 해당 job의 결과/provider에 접근하지 않는 것이 핵심 계약이다. */
#include "event_server.h"
#include "proxy_internal.h"
#include "server.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/uio.h>

enum {
    ACCEPT_QUEUE_CAPACITY = 256,
    WORKER_CONNECTION_LIMIT = 4096,
    LOCAL_JOB_CAPACITY = 256,
    LOCAL_THREAD_COUNT = 2,
    EVENT_BATCH_SIZE = 128,
    CONNECTION_STEP_BUDGET = 64
};

struct event_connection;
struct event_worker;
struct event_runtime;

enum connection_phase {
    READ_CLIENT_HEADER, SEND_CONTINUE, READ_CLIENT_BODY,
    CONNECT_BACKEND, SEND_BACKEND_REQUEST, READ_BACKEND_HEADER,
    SEND_CLIENT_HEADER, READ_BACKEND_BODY, SEND_CLIENT_BODY,
    WAIT_LOCAL_PREPARE, WAIT_LOCAL_BODY, WAIT_LOCAL_CLEANUP,
    DRAIN_CLIENT, CONNECTION_CLOSED
};
enum backend_chunk_phase { READ_CHUNK_SIZE, READ_CHUNK_DATA, READ_CHUNK_CRLF, READ_CHUNK_TRAILERS };
enum local_job_kind { PREPARE_LOCAL_RESPONSE, READ_LOCAL_BODY, CLEANUP_LOCAL_RESPONSE };

struct event_endpoint {
    struct event_connection *connection;
    int fd;
    uint32_t interest; /* 0 means not registered; HUP must not spin on inactive FDs. */
    bool backend;
};

struct pending_output {
    struct iovec parts[3];
    size_t count, index, offset;
};

struct local_job {
    struct event_connection *connection;
    struct local_job *next;
    enum local_job_kind kind;
    enum body_result result;
    struct http_slice chunk;
    int error;
};

struct local_work_pool {
    pthread_mutex_t mutex;
    pthread_cond_t available;
    pthread_t threads[LOCAL_THREAD_COUNT];
    unsigned int started;
    struct local_job *head, *tail;
    size_t queued;
    bool stopping;
};

struct accepted_client { int fd; int64_t accepted_at; };

struct event_worker {
    struct event_runtime *runtime;
    pthread_t thread;
    bool started;
    int epoll_fd, wake_fd;
    pthread_mutex_t inbox_mutex;
    struct accepted_client accepted[ACCEPT_QUEUE_CAPACITY];
    size_t accept_head, accept_count;
    struct local_job *completed_head, *completed_tail;
    struct local_job *waiting_head, *waiting_tail;
    atomic_size_t reserved; /* 큐 + 살아 있는 연결. 관리자는 이 숫자만 읽는다. */
    atomic_bool stopping;
    bool shutdown_started;
    struct event_connection *connections;
    struct event_connection *ready_head, *ready_tail, *retired;
    struct event_connection *timers[WORKER_CONNECTION_LIMIT];
    size_t timer_count;
};

struct event_runtime {
    const struct proxy_config *config;
    struct proxy_selection_state selection;
    pthread_mutex_t selection_mutex;
    struct local_work_pool local_pool;
    struct event_worker *workers;
    unsigned int worker_count;
    atomic_bool failed;
    int failure_fd;
};

struct event_connection {
    struct event_worker *worker;
    struct event_connection *previous, *next, *ready_next, *retired_next;
    struct event_endpoint client, backend;
    enum connection_phase phase;
    bool ready_queued, retired, response_started, head_request, closing;
    bool local_job_inflight, local_cleanup_done;
    size_t heap_index;
    int64_t timer_at, stage_deadline, body_deadline, backend_idle_deadline, write_deadline;

    /* 파서의 slice는 이 메모리를 빌린다. 함수가 반환되어도 연결과 함께 살아 있다. */
    unsigned char request_header[REQ_HEADER_MAX_BYTES];
    size_t header_received, header_end, body_received;
    unsigned char *request_body;
    struct http_request request;
    struct request_route route;
    const char *allow;

    struct router_response response;
    size_t response_remaining;
    struct local_job job; /* 한 연결은 한 번에 보조 작업 하나만 가질 수 있다. */
    struct pending_output output;
    char local_header[512], chunk_prefix[32];
    bool output_is_last;
    size_t drained;

    /* 프록시 연결에만 지연 할당한다. 가만히 연결만 한 클라이언트에는 만들지 않는다. */
    struct proxy_stream *stream;
    struct proxy_response_headers *backend_headers;
    unsigned char *backend_request_header;
    struct sockaddr_in backend_address;
    enum backend_chunk_phase chunk_phase;
    unsigned char *chunk_line, *trailer_bytes;
    size_t chunk_line_length, chunk_crlf_received, trailer_length;
};

int64_t monotonic_milliseconds(void);
void notify_event_fd(int fd);
int start_local_work_pool(struct local_work_pool *pool);
void stop_local_work_pool(struct local_work_pool *pool);
bool submit_local_job(struct event_connection *connection, enum local_job_kind kind);
void submit_waiting_local_jobs(struct event_worker *worker);
void complete_local_job(struct event_connection *connection);

int initialize_event_worker(struct event_worker *worker, struct event_runtime *runtime);
void destroy_event_worker(struct event_worker *worker);
void *run_worker_event_loop(void *argument);
bool assign_client_to_worker(struct event_worker *worker, struct accepted_client client);
void schedule_connection(struct event_connection *connection);
void update_connection_timer(struct event_connection *connection);
void forget_connection_timer(struct event_connection *connection);
void retire_connection(struct event_connection *connection);

struct event_connection *create_event_connection(struct event_worker *, struct accepted_client);
void advance_connection(struct event_connection *connection);
void handle_connection_event(struct event_endpoint *endpoint, uint32_t events);
void expire_connection(struct event_connection *connection);
void close_event_connection(struct event_connection *connection);
void release_event_connection(struct event_connection *connection);
void refresh_connection_interest(struct event_connection *connection);
int64_t connection_next_deadline(const struct event_connection *connection);
void set_connection_output(struct event_connection *, struct http_slice,
                            struct http_slice, struct http_slice);
void prepare_connection_error(struct event_connection *, int status);
void start_client_response(struct event_connection *connection);
void prepare_client_body_chunk(struct event_connection *, struct http_slice chunk);
void finish_client_body(struct event_connection *connection);
void begin_client_drain(struct event_connection *connection);
void detach_endpoint(struct event_connection *, struct event_endpoint *endpoint);

int start_backend_request(struct event_connection *connection);
/* 1: phase/data progressed; 0: EAGAIN, await readiness; -1: errno. */
int advance_backend_connection(struct event_connection *connection);
void fail_backend_connection(struct event_connection *connection, int error);

#endif
