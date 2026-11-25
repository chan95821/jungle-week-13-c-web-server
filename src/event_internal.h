#ifndef EVENT_INTERNAL_H
#define EVENT_INTERNAL_H

/* B 모델 내부에서만 공유하는 타입. 연결의 단계·소켓·출력 위치는 소유 worker만 수정한다.
 * 보조 스레드에 맡긴 response/provider는 결과를 돌려받을 때까지 owner가 접근하지 않는다. */
#include "event_server.h"
#include "proxy_internal.h"
#include "server.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/uio.h>

/* 요청마다 스레드/버퍼를 무한히 늘리지 않기 위한 상한이다. 실제 수용량은 OS FD 한도에도 제한된다. */
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

/* advance_connection의 다음 행동을 정한다. 함수가 EAGAIN으로 반환돼도 이 값은 연결에 남는다. */
enum connection_phase {
    READ_CLIENT_HEADER, SEND_CONTINUE, READ_CLIENT_BODY,
    CONNECT_BACKEND, SEND_BACKEND_REQUEST, READ_BACKEND_HEADER,
    SEND_CLIENT_HEADER, READ_BACKEND_BODY, SEND_CLIENT_BODY,
    WAIT_LOCAL_PREPARE, WAIT_LOCAL_BODY, WAIT_LOCAL_CLEANUP,
    DRAIN_CLIENT, CONNECTION_CLOSED
};
/* chunked 본문의 한 줄/CRLF가 여러 recv로 나뉘어 도착해도 중간 위치를 유지한다. */
enum backend_chunk_phase { READ_CHUNK_SIZE, READ_CHUNK_DATA, READ_CHUNK_CRLF, READ_CHUNK_TRAILERS };
/* 보조 풀은 로컬 응답 준비·다음 본문 조각 읽기·자원 정리만 맡는다. */
enum local_job_kind { PREPARE_LOCAL_RESPONSE, READ_LOCAL_BODY, CLEANUP_LOCAL_RESPONSE };

/* 부분 송신 결과다. 기존 -1/0/1/2 의미를 이름으로 드러내며 I/O 대기와 완료를 구분한다. */
enum send_result {
    SEND_FAILED = -1,
    SEND_WOULD_BLOCK = 0,
    SEND_FINISHED = 1,
    SEND_CAN_CONTINUE = 2
};

/* epoll_event.data.ptr에 넣는 연결의 한쪽 끝이다. 이벤트를 받으면 소유 연결로 돌아간다. */
struct event_endpoint {
    struct event_connection *connection;
    int fd;
    uint32_t interest; /* 현재 감시 비트. 0은 미등록이며 FD를 닫았다는 뜻은 아니다. */
    bool backend; /* false=클라이언트 소켓, true=백엔드 소켓 */
};

/* 전송할 메모리를 빌려 둔 최대 세 조각. sendmsg가 덜 보내면 index/offset부터 재개한다.
 * 이 조각을 전부 보내기 전에는 원본 수신 버퍼/provider를 다시 사용하면 안 된다. */
struct pending_output {
    struct iovec parts[3];
    size_t count, index, offset;
};

/* 연결마다 하나를 내장한다. 큐/실행/완료 중에도 연결 메모리를 유지해 포인터 수명을 보장한다. */
struct local_job {
    struct event_connection *connection;
    struct local_job *next;
    enum local_job_kind kind;
    enum body_result result;
    struct http_slice chunk;
    int error;
};

/* 네트워크 작업자들이 제출하고 고정 보조 스레드들이 꺼내는 작업 큐. mutex/조건변수로 동기화한다. */
struct local_work_pool {
    pthread_mutex_t mutex;
    pthread_cond_t available;
    pthread_t threads[LOCAL_THREAD_COUNT];
    unsigned int started;
    struct local_job *head, *tail;
    size_t queued;
    bool stopping;
};

/* 메인 → 네트워크 작업자로 전달하는 정보다. 커널의 FD 테이블 자체를 복사하는 구조체가 아니다. */
struct accepted_client { int fd; int64_t accepted_at; };

/* 작업자 한 명의 실행 환경. epoll/wake FD 번호는 프로세스 전체 FD 테이블을 가리키지만,
 * 연결 목록·실행 대기 큐·타이머는 소유 worker만 수정한다. */
struct event_worker {
    struct event_runtime *runtime;
    pthread_t thread;
    bool started;
    int epoll_fd, wake_fd; /* FD 감시 인스턴스 / 공유 큐 확인을 요청하는 알림 */
    pthread_mutex_t inbox_mutex;
    /* 아래 accepted/completed 두 큐는 외부 스레드도 쓰므로 inbox_mutex가 필요하다. */
    struct accepted_client accepted[ACCEPT_QUEUE_CAPACITY];
    size_t accept_head, accept_count;
    struct local_job *completed_head, *completed_tail;
    struct local_job *waiting_head, *waiting_tail; /* 보조 풀 큐가 가득 차 잠시 보관한 작업 */
    atomic_size_t reserved; /* 인수 대기 + 정리 전 클라이언트 연결 수. 백엔드 FD 수는 더하지 않는다. */
    atomic_bool stopping;
    bool shutdown_started;
    struct event_connection *connections; /* 종료 때 모든 소유 연결을 찾는 활성 연결 목록 */
    struct event_connection *ready_head, *ready_tail, *retired; /* 즉시 실행 대기 / 배치 후 free 대기 */
    struct event_connection *timers[WORKER_CONNECTION_LIMIT]; /* 가장 빠른 기한이 맨 앞인 최소 힙 */
    size_t timer_count;
};

/* 모든 작업자가 함께 참조하는 실행 정보. config는 불변이고 조회 선택 순번만 mutex로 보호한다. */
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

/* 클라이언트 연결 하나의 수명 전체를 보관한다. 새 요청마다 스레드를 만드는 대신 이 상태를 만든다.
 * 네트워크 I/O/단계 전환은 owner만 수행하고, 보조 작업 중 해당 결과/provider 접근은 잠시 넘긴다. */
struct event_connection {
    struct event_worker *worker;
    struct event_connection *previous, *next, *ready_next, *retired_next;
    struct event_endpoint client, backend;
    enum connection_phase phase;
    bool ready_queued, retired, response_started, head_request, closing;
    bool local_job_inflight, local_cleanup_done;
    atomic_bool cancel_local_job;
    size_t heap_index; /* 타이머 힙 안의 위치. SIZE_MAX이면 등록되지 않았다. */
    int64_t timer_at, stage_deadline, body_deadline, client_idle_deadline;
    int64_t backend_idle_deadline, write_deadline;

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
    struct pending_output output; /* client/backend 중 현재 보낼 대상의 미완료 데이터 */
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
enum send_result send_pending_output(struct event_connection *, struct event_endpoint *);
void prepare_connection_error(struct event_connection *, int status);
void start_client_response(struct event_connection *connection);
void prepare_client_body_chunk(struct event_connection *, struct http_slice chunk);
void finish_client_body(struct event_connection *connection);
void begin_client_drain(struct event_connection *connection);
void detach_endpoint(struct event_connection *, struct event_endpoint *endpoint);

int start_backend_request(struct event_connection *connection);
/* 1=단계/데이터 처리를 이어갈 수 있음, 0=EAGAIN으로 이벤트 대기, -1=errno 오류. */
int advance_backend_connection(struct event_connection *connection);
void fail_backend_connection(struct event_connection *connection, int error);

#endif
