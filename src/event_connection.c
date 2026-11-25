#define _GNU_SOURCE
#include "event_internal.h"
#include "handlers/handlers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 연결 하나의 흐름: 헤더 수신/검증 → 본문 수신 → dispatch_complete_request
 * → 백엔드 또는 로컬 응답 준비 → 헤더/본문 송신 → 배출/정리.
 * advance_connection이 phase를 보고 필요한 함수를 부른다. EAGAIN이면 상태를 남기고
 * worker 루프로 반환하며, 다음 이벤트가 같은 connection으로 이 함수를 다시 호출한다. */

static void dispatch_complete_request(struct event_connection *connection);
static void request_next_local_body(struct event_connection *connection);

/* [소유 worker] 인수한 client FD를 연결 상태와 묶는다. 이 구조체가 요청/송신 위치를 기억한다.
 * 아직 소켓을 epoll에 넣지 않고 실행 큐에 올려, 이미 도착한 요청부터 바로 읽어 본다. */
struct event_connection *create_event_connection(struct event_worker *worker,
                                                  struct accepted_client accepted) {
    struct event_connection *connection = calloc(1, sizeof(*connection));
    if (!connection) return NULL;
    connection->worker = worker;
    atomic_init(&connection->cancel_local_job, false);
    connection->client = (struct event_endpoint){connection, accepted.fd, 0, false};
    /* 백엔드는 HTTP 경로와 본문 검증 후 연결하므로 처음에는 유효한 FD가 없다. */
    connection->backend = (struct event_endpoint){connection, -1, 0, true};
    connection->phase = READ_CLIENT_HEADER;
    connection->heap_index = SIZE_MAX;
    /* 큐에서 기다린 시간도 헤더 기한에 포함한다. 요청 버퍼와 기한을 전역에 두지 않는다. */
    connection->stage_deadline = accepted.accepted_at + HEADER_DEADLINE_SECONDS * 1000;
    connection->client_idle_deadline = accepted.accepted_at + IO_CALL_TIMEOUT_SECONDS * 1000;
    connection->next = worker->connections;
    if (worker->connections) worker->connections->previous = connection;
    worker->connections = connection;
    schedule_connection(connection);
    return connection;
}

/* 감시 등록을 제거하고 소켓을 닫는다. fd=-1을 남겨 다른 정리 경로에서 중복 close하지 않는다. */
void detach_endpoint(struct event_connection *connection, struct event_endpoint *endpoint) {
    if (endpoint->fd < 0) return;
    if (endpoint->interest)
        epoll_ctl(connection->worker->epoll_fd, EPOLL_CTL_DEL, endpoint->fd, NULL);
    close(endpoint->fd); /* Linux close는 EINTR이어도 재시도하지 않는다. */
    endpoint->fd = -1;
    endpoint->interest = 0;
    if (endpoint->backend && connection->stream) connection->stream->backend_fd = -1;
}

/* 연결 종료 요청의 공통 진입점. FD를 먼저 회수하고 보조 작업/cleanup이 끝난 뒤 retire한다.
 * 이 함수에서 연결 메모리를 바로 free하지 않아 같은 epoll 배치의 이벤트 포인터를 보호한다. */
void close_event_connection(struct event_connection *connection) {
    if (connection->retired) return;
    connection->closing = true;
    /* 보조 풀에 이미 들어간 작업도 실행 직전에 이 표시를 보고 준비/읽기를 건너뛸 수 있다. */
    atomic_store(&connection->cancel_local_job, true);
    forget_connection_timer(connection);
    detach_endpoint(connection, &connection->client);
    detach_endpoint(connection, &connection->backend);
    /* 보조 스레드가 request/response를 만지고 있으면 메모리/상태를 건드리지 않는다. */
    if (connection->local_job_inflight) return;
    /* 파일 close/CGI 자식 회수 역시 대기할 수 있으므로 네트워크 worker에서 직접 하지 않는다. */
    if (connection->response.body_provider.cleanup_func && !connection->local_cleanup_done) {
        connection->phase = WAIT_LOCAL_CLEANUP;
        submit_local_job(connection, CLEANUP_LOCAL_RESPONSE);
        return;
    }
    retire_connection(connection);
}

/* 이벤트 배치와 보조 작업이 더 이상 참조하지 않을 때 호출하는 최종 메모리 해제.
 * 소켓은 앞의 close 경로에서 닫혔고, proxy_stream에도 backend_fd=-1이 반영돼 있다. */
void release_event_connection(struct event_connection *connection) {
    if (connection->stream) cleanup_backend_response(connection->stream);
    free(connection->backend_headers);
    free(connection->backend_request_header);
    free(connection->chunk_line);
    free(connection->trailer_bytes);
    free(connection->request_body);
    free(connection);
}

/* 같은 감시 설정이면 시스템 호출을 생략한다. interest=0은 감시만 제거하고 FD는 유지한다.
 * 이벤트에는 endpoint 주소를 넣어 epoll_wait에서 소유 연결을 바로 찾을 수 있게 한다. */
static int set_endpoint_interest(struct event_connection *connection,
                                 struct event_endpoint *endpoint, uint32_t interest) {
    if (endpoint->fd < 0 || endpoint->interest == interest) return 0;
    if (!interest) {
        if (endpoint->interest &&
            epoll_ctl(connection->worker->epoll_fd, EPOLL_CTL_DEL, endpoint->fd, NULL) < 0)
            return -1;
    } else {
        struct epoll_event event = {.events = interest, .data.ptr = endpoint};
        int operation = endpoint->interest ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (epoll_ctl(connection->worker->epoll_fd, operation, endpoint->fd, &event) < 0) return -1;
    }
    endpoint->interest = interest;
    return 0;
}

/* 현재 단계가 기다리는 쪽만 감시한다. 예: 백엔드 수신 중에는 backend EPOLLIN,
 * 느린 클라이언트 송신 중에는 client EPOLLOUT. 불필요한 HUP/쓰기 알림 반복도 줄인다. */
void refresh_connection_interest(struct event_connection *connection) {
    if (connection->closing || connection->retired) return;
    uint32_t client = 0, backend = 0;
    switch (connection->phase) {
        case READ_CLIENT_HEADER: case READ_CLIENT_BODY: case DRAIN_CLIENT:
            client = EPOLLIN | EPOLLRDHUP; break;
        case SEND_CONTINUE: case SEND_CLIENT_HEADER: case SEND_CLIENT_BODY:
            client = EPOLLOUT; break;
        case CONNECT_BACKEND: case SEND_BACKEND_REQUEST:
            backend = EPOLLOUT; break;
        case READ_BACKEND_HEADER: case READ_BACKEND_BODY:
            backend = EPOLLIN | EPOLLRDHUP; break;
        default: break;
    }
    /* 출력이 없을 때 EPOLLOUT을 켜 두면 '쓸 수 있다' 이벤트가 계속 발생한다. */
    if (set_endpoint_interest(connection, &connection->client, client) < 0 ||
        set_endpoint_interest(connection, &connection->backend, backend) < 0)
        close_event_connection(connection);
}

/* 현재 단계의 전체 기한/idle 기한 중 빠른 것을 반환한다. -1이면 worker 타이머가 필요 없다.
 * 보조 파일/CGI 작업은 해당 provider의 대기 제한을 보조 스레드에서 적용한다. */
int64_t connection_next_deadline(const struct event_connection *connection) {
    if (connection->closing || connection->retired) return -1;
    switch (connection->phase) {
        case READ_CLIENT_HEADER: case READ_CLIENT_BODY:
            return connection->stage_deadline < connection->client_idle_deadline
                ? connection->stage_deadline : connection->client_idle_deadline;
        case CONNECT_BACKEND:
        case SEND_BACKEND_REQUEST: case READ_BACKEND_HEADER: case DRAIN_CLIENT:
            return connection->stage_deadline;
        case SEND_CONTINUE:
            return connection->write_deadline < connection->stage_deadline
                ? connection->write_deadline : connection->stage_deadline;
        case SEND_CLIENT_HEADER: case SEND_CLIENT_BODY:
            if (connection->stream && connection->body_deadline < connection->write_deadline)
                return connection->body_deadline;
            return connection->write_deadline;
        case READ_BACKEND_BODY:
            return connection->body_deadline < connection->backend_idle_deadline
                ? connection->body_deadline : connection->backend_idle_deadline;
        default: return -1; /* 파일/CGI의 대기는 기존 provider 제한을 보조 풀에서 적용한다. */
    }
}

/* 최대 세 메모리 조각을 전송 대상으로 설정하고 송신 위치/idle 기한을 초기화한다.
 * 헤더+본문 또는 chunk 크기+본문+CRLF를 합쳐 복사하지 않고 sendmsg로 보낼 준비다. */
void set_connection_output(struct event_connection *connection, struct http_slice first,
                            struct http_slice second, struct http_slice third) {
    connection->output = (struct pending_output){0};
    struct http_slice slices[] = {first, second, third};
    for (size_t i = 0; i < 3; ++i) {
        if (!slices[i].length) continue;
        connection->output.parts[connection->output.count++] =
            (struct iovec){(void *)slices[i].data, slices[i].length};
    }
    connection->write_deadline = monotonic_milliseconds() + IO_CALL_TIMEOUT_SECONDS * 1000;
}

/* client 또는 backend에 가능한 만큼만 보낸다. 원본 조각은 SEND_FINISHED까지 유지한다.
 * SEND_CAN_CONTINUE면 즉시 재시도 가능, SEND_WOULD_BLOCK이면 epoll 알림을 기다린다. */
enum send_result send_pending_output(struct event_connection *connection, struct event_endpoint *endpoint) {
    struct pending_output *output = &connection->output;
    if (output->index == output->count) return SEND_FINISHED;
    struct iovec parts[3];
    size_t count = output->count - output->index;
    /* 이전 send가 끝난 조각/바이트는 제외한 뒤 미완료 구간만 시스템 호출에 넘긴다. */
    for (size_t i = 0; i < count; ++i) parts[i] = output->parts[output->index + i];
    parts[0].iov_base = (unsigned char *)parts[0].iov_base + output->offset;
    parts[0].iov_len -= output->offset;
    struct msghdr message = {.msg_iov = parts, .msg_iovlen = count};
    ssize_t sent = sendmsg(endpoint->fd, &message, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return SEND_WOULD_BLOCK;
        if (errno == EINTR) return SEND_CAN_CONTINUE;
        return SEND_FAILED;
    }
    if (sent == 0) { errno = EIO; return SEND_FAILED; }
    /* 100 Continue를 제외한 응답 바이트를 보내기 시작하면 새 오류 응답을 덧붙일 수 없다. */
    if (!endpoint->backend && connection->phase != SEND_CONTINUE)
        connection->response_started = true;
    connection->write_deadline = monotonic_milliseconds() + IO_CALL_TIMEOUT_SECONDS * 1000;
    /* 실제로 보낸 바이트만 위치에 반영한다. 다음 이벤트에서도 중복/누락 없이 이어 보낸다. */
    size_t remaining = (size_t)sent;
    while (remaining) {
        size_t room = output->parts[output->index].iov_len - output->offset;
        size_t consumed = remaining < room ? remaining : room;
        output->offset += consumed;
        remaining -= consumed;
        if (output->offset == output->parts[output->index].iov_len) {
            ++output->index;
            output->offset = 0;
        }
    }
    return output->index == output->count ? SEND_FINISHED : SEND_CAN_CONTINUE;
}

/* 응답 전이면 로컬 오류 응답을 준비한다. 이미 응답 중이면 닫아서 불완전함을 드러낸다. */
void prepare_connection_error(struct event_connection *connection, int status) {
    if (connection->response_started) { close_event_connection(connection); return; }
    router_prepare_error(&connection->response, status, connection->allow);
    start_client_response(connection);
}

/* 기존 로컬 응답의 상태/길이/chunked/Allow를 bounded 헤더에 쓴다.
 * HEAD 오류 응답은 기존 계약대로 본문과 길이 프레이밍을 생략한다. 반환은 길이 또는 -1이다. */
static int format_local_response_header(struct event_connection *connection) {
    struct router_response *response = &connection->response;
    char allow[32] = "";
    if (response->allow) {
        int size = snprintf(allow, sizeof(allow), "Allow: %s\r\n", response->allow);
        if (size < 0 || (size_t)size >= sizeof(allow)) return -1;
    }
    char framing[64] = "";
    if (!connection->head_request) {
        int size = response->chunked
            ? snprintf(framing, sizeof(framing), "Transfer-Encoding: chunked\r\n")
            : snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", response->length);
        if (size < 0 || (size_t)size >= sizeof(framing)) return -1;
    }
    int size = snprintf(connection->local_header, sizeof(connection->local_header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%s%sConnection: close\r\n\r\n",
        response->status, http_reason(response->status), response->content_type, framing, allow);
    if (size < 0 || (size_t)size >= sizeof(connection->local_header)) return -1;
    return size;
}

/* 준비된 응답을 실제 송신 단계로 연결한다. 백엔드가 만든 wire_header가 있으면 재사용하고,
 * 로컬 응답이면 헤더를 만든다. 아직 send하지 않고 SEND_CLIENT_HEADER와 출력 조각을 설정한다. */
void start_client_response(struct event_connection *connection) {
    struct router_response *response = &connection->response;
    struct http_slice header = response->wire_header;
    if (!header.length) {
        int length = format_local_response_header(connection);
        if (length < 0) { close_event_connection(connection); return; }
        header = (struct http_slice){(unsigned char *)connection->local_header, (size_t)length};
    }
    connection->response_remaining = response->length;
    connection->phase = SEND_CLIENT_HEADER;
    set_connection_output(connection, header, (struct http_slice){0}, (struct http_slice){0});
}

/* 다음 본문 조각을 검증하고 송신 대상으로 고정한다. fixed면 남은 길이,
 * chunked면 크기 줄/본문/CRLF를 준비한다. 모두 보낼 때까지 producer를 다시 호출하지 않는다. */
void prepare_client_body_chunk(struct event_connection *connection, struct http_slice chunk) {
    if (!chunk.data || !chunk.length ||
        (!connection->response.chunked && chunk.length > connection->response_remaining)) {
        close_event_connection(connection);
        return;
    }
    connection->output_is_last = false;
    connection->phase = SEND_CLIENT_BODY;
    if (connection->response.chunked) {
        int length = snprintf(connection->chunk_prefix, sizeof(connection->chunk_prefix), "%zx\r\n", chunk.length);
        if (length < 0 || (size_t)length >= sizeof(connection->chunk_prefix)) {
            close_event_connection(connection); return;
        }
        set_connection_output(connection,
            (struct http_slice){(unsigned char *)connection->chunk_prefix, (size_t)length}, chunk,
            (struct http_slice){(const unsigned char *)"\r\n", 2});
    } else {
        connection->response_remaining -= chunk.length;
        connection->output_is_last = connection->response_remaining == 0;
        set_connection_output(connection, chunk, (struct http_slice){0}, (struct http_slice){0});
    }
}

/* producer의 정상 종료를 응답 종료로 연결한다. fixed 길이 부족은 실패이고,
 * chunked의 정상 종료 조각과 검증된 trailer는 이 성공 경로에서만 송신한다. */
void finish_client_body(struct event_connection *connection) {
    if (!connection->response.chunked) {
        if (connection->response_remaining) close_event_connection(connection);
        else begin_client_drain(connection);
        return;
    }
    struct http_slice trailers = connection->stream ? connection->stream->trailers :
        connection->response.body_provider.trailers ? *connection->response.body_provider.trailers :
        (struct http_slice){0};
    connection->phase = SEND_CLIENT_BODY;
    connection->output_is_last = true;
    /* 이 지점은 본문/CGI 종료/트레일러 검증에 성공했을 때만 도달한다. */
    set_connection_output(connection, (struct http_slice){(const unsigned char *)"0\r\n", 3},
                            trailers, (struct http_slice){(const unsigned char *)"\r\n", 2});
}

/* 응답 완료: backend를 먼저 닫고 client의 송신 방향만 종료한다.
 * 남은 client 입력은 DRAIN_CLIENT에서 최대 1초/64KiB만 읽은 뒤 최종 close한다. */
void begin_client_drain(struct event_connection *connection) {
    detach_endpoint(connection, &connection->backend);
    if (shutdown(connection->client.fd, SHUT_WR) < 0) {
        close_event_connection(connection); return;
    }
    connection->phase = DRAIN_CLIENT;
    connection->stage_deadline = monotonic_milliseconds() + DRAIN_DEADLINE_SECONDS * 1000;
}

/* 완성 헤더 → HTTP 파싱 → 경로/메서드 선택 → 프록시 보호 헤더 검사 순으로 진행한다.
 * 실패는 본문 수신/100 Continue/백엔드 접속보다 먼저 응답하여 오류 우선순위를 유지한다. */
static void process_received_request_header(struct event_connection *connection) {
    connection->stage_deadline = monotonic_milliseconds() + BODY_DEADLINE_SECONDS * 1000;
    int status = http_parse_request(connection->request_header, connection->header_end, &connection->request);
    connection->head_request = http_slice_equal(connection->request.method, "HEAD");
    if (!status) status = http_select_route(&connection->request, &connection->route, &connection->allow);
    if (!status && connection->route.proxy_route != PROXY_ROUTE_LOCAL)
        status = proxy_validate_request(&connection->request);
    if (status) { prepare_connection_error(connection, status); return; }
    /* 헤더와 같은 recv에 들어온 본문부터 복사한다. 그 뒤 선언된 길이까지만 더 받는다. */
    if (connection->request.content_length) {
        connection->request_body = malloc(connection->request.content_length);
        if (!connection->request_body) { prepare_connection_error(connection, 500); return; }
        size_t received = connection->header_received - connection->header_end;
        if (received > connection->request.content_length) received = connection->request.content_length;
        memcpy(connection->request_body, connection->request_header + connection->header_end, received);
        connection->body_received = received;
    }
    connection->phase = READ_CLIENT_BODY;
    /* 클라이언트가 본문 송신 허락을 기다릴 때만 interim을 별도 송신 단계에 올린다. */
    if (connection->request.expect_continue &&
        connection->body_received < connection->request.content_length) {
        connection->phase = SEND_CONTINUE;
        set_connection_output(connection,
            (struct http_slice){(const unsigned char *)"HTTP/1.1 100 Continue\r\n\r\n", 25},
            (struct http_slice){0}, (struct http_slice){0});
    }
}

/* recv 경계에 걸린 CRLFCRLF까지 이어 찾는다. 1=다시 진행 가능, 0=대기/종료다.
 * 실제 길이만 누적하므로 NUL 종료 문자열에 의존하지 않고 동반 본문도 보존한다. */
static int read_client_request_headers(struct event_connection *connection) {
    size_t previous = connection->header_received;
    if (previous == sizeof(connection->request_header)) {
        prepare_connection_error(connection, 431); return 1;
    }
    ssize_t count = recv(connection->client.fd, connection->request_header + previous,
                          sizeof(connection->request_header) - previous, 0);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 1;
        close_event_connection(connection); return 0;
    }
    if (count == 0) {
        if (previous) prepare_connection_error(connection, 400);
        else close_event_connection(connection);
        return 1;
    }
    connection->header_received += (size_t)count;
    connection->client_idle_deadline = monotonic_milliseconds() + IO_CALL_TIMEOUT_SECONDS * 1000;
    /* 이전 recv의 마지막 세 바이트부터 재검사해야 두 recv 사이의 구분자를 놓치지 않는다. */
    size_t start = previous > 3 ? previous - 3 : 0;
    for (size_t i = start; i + 4 <= connection->header_received; ++i) {
        if (memcmp(connection->request_header + i, "\r\n\r\n", 4) == 0) {
            connection->header_end = i + 4;
            process_received_request_header(connection);
            break;
        }
    }
    return 1;
}

/* Content-Length만큼 본문을 누적한다. 완료 시 dispatch로 넘기며 GET의 0바이트도 여기서 완료된다.
 * 조기 EOF는 400, EAGAIN은 실패가 아니라 worker 루프로 돌아갈 대기 신호다. */
static int read_client_request_body(struct event_connection *connection) {
    if (connection->body_received == connection->request.content_length) {
        connection->request.body = (struct http_slice){connection->request_body, connection->body_received};
        dispatch_complete_request(connection);
        return 1;
    }
    ssize_t count = recv(connection->client.fd, connection->request_body + connection->body_received,
                          connection->request.content_length - connection->body_received, 0);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 1;
        close_event_connection(connection); return 0;
    }
    if (count == 0) { prepare_connection_error(connection, 400); return 1; }
    connection->body_received += (size_t)count;
    connection->client_idle_deadline = monotonic_milliseconds() + IO_CALL_TIMEOUT_SECONDS * 1000;
    return 1;
}

/* 요청 전체를 받은 뒤 목적지별 처리를 시작한다. API는 event_proxy로,
 * 짧은 메모리 응답은 기존 handler로, 파일/CGI는 대기 가능한 보조 작업으로 넘긴다. */
static void dispatch_complete_request(struct event_connection *connection) {
    if (connection->route.proxy_route != PROXY_ROUTE_LOCAL) {
        if (start_backend_request(connection) < 0) fail_backend_connection(connection, errno);
        return;
    }
    router_handler handler = connection->route.local_handler;
    /* 이 세 핸들러는 디스크/파이프 대기가 없는 짧은 메모리 연산이다. */
    if (handler == prepare_hello || handler == prepare_echo || handler == prepare_greet) {
        handler(&connection->request, &connection->response);
        start_client_response(connection);
    } else {
        connection->phase = WAIT_LOCAL_PREPARE;
        submit_local_job(connection, PREPARE_LOCAL_RESPONSE);
    }
}

/* 로컬 producer의 결과를 네트워크 단계로 변환한다: DATA=송신, END=정상 종료, ERROR=중단. */
static void consume_local_body_result(struct event_connection *connection,
                                      enum body_result result, struct http_slice chunk) {
    if (result == BODY_DATA) prepare_client_body_chunk(connection, chunk);
    else if (result == BODY_END) finish_client_body(connection);
    else close_event_connection(connection);
}

/* 이전 출력 조각을 전부 보낸 뒤에만 다음 조각을 요청한다.
 * 빌린 메모리는 바로 사용하고, 파일/CGI 등 provider 호출은 보조 풀에 맡긴다. */
static void request_next_local_body(struct event_connection *connection) {
    if (!connection->response.chunked && !connection->response_remaining) {
        begin_client_drain(connection); return;
    }
    if (!connection->response.body_provider.next_func) { close_event_connection(connection); return; }
    if (connection->response.body) {
        struct http_slice chunk = {0};
        enum body_result result = connection->response.body_provider.next_func(
            connection->response.body_provider.state, &chunk);
        consume_local_body_result(connection, result, chunk);
    } else {
        connection->phase = WAIT_LOCAL_BODY;
        submit_local_job(connection, READ_LOCAL_BODY);
    }
}

/* [보조 스레드가 아니라 소유 worker에서 호출] inbox로 돌아온 결과를 반영한다.
 * 이제 provider 접근권을 돌려받으며, 그동안 종료된 연결이면 송신 대신 정리만 이어간다. */
void complete_local_job(struct event_connection *connection) {
    enum local_job_kind kind = connection->job.kind;
    connection->local_job_inflight = false;
    if (kind == CLEANUP_LOCAL_RESPONSE) {
        connection->response.body_provider = (struct body_provider){0};
        connection->local_cleanup_done = true;
    }
    if (connection->closing) { close_event_connection(connection); return; }
    if (kind == PREPARE_LOCAL_RESPONSE) start_client_response(connection);
    else if (kind == READ_LOCAL_BODY)
        consume_local_body_result(connection, connection->job.result, connection->job.chunk);
    schedule_connection(connection);
}

/* SHUT_WR 뒤 남은 입력을 버린다. EOF/오류/바이트 상한이면 close하고 EAGAIN이면 읽기 알림을 기다린다.
 * 시간 상한은 별도 worker 타이머가 적용하므로 여기서 blocking recv를 반복하지 않는다. */
static int drain_client_input(struct event_connection *connection) {
    unsigned char discard[1024];
    size_t room = DRAIN_MAX_BYTES - connection->drained;
    if (!room) { close_event_connection(connection); return 0; }
    if (room > sizeof(discard)) room = sizeof(discard);
    ssize_t count = recv(connection->client.fd, discard, room, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (count < 0 && errno == EINTR) return 1;
    if (count <= 0) { close_event_connection(connection); return 0; }
    connection->drained += (size_t)count;
    return 1;
}

/* 만료된 단계에 맞춰 조치한다: 요청 수신 408, 백엔드 대기 504(응답 중이면 close),
 * 클라이언트 송신/배출 기한은 close. 준비한 오류 응답도 다시 실행 큐에 올려 진행한다. */
void expire_connection(struct event_connection *connection) {
    switch (connection->phase) {
        case READ_CLIENT_HEADER: case READ_CLIENT_BODY:
            prepare_connection_error(connection, 408); break;
        case CONNECT_BACKEND: case SEND_BACKEND_REQUEST: case READ_BACKEND_HEADER:
        case READ_BACKEND_BODY:
            fail_backend_connection(connection, ETIMEDOUT); break;
        default: close_event_connection(connection); break;
    }
    refresh_connection_interest(connection);
    update_connection_timer(connection);
    schedule_connection(connection);
}

/* epoll이 돌려준 endpoint에서 연결을 찾아 재개한다. 같은 배치의 앞 이벤트가 닫았을 수 있어 먼저 확인한다.
 * CONNECT_BACKEND의 EPOLLOUT은 성공 보장이 아니므로 SO_ERROR를 확인한 뒤 요청 송신으로 넘어간다. */
void handle_connection_event(struct event_endpoint *endpoint, uint32_t events) {
    struct event_connection *connection = endpoint->connection;
    if (connection->closing || connection->retired || endpoint->fd < 0) return;
    if (endpoint->backend && connection->phase == CONNECT_BACKEND) {
        if (monotonic_milliseconds() >= connection->stage_deadline) {
            fail_backend_connection(connection, ETIMEDOUT);
            advance_connection(connection);
            return;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(endpoint->fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) error = errno;
        if (error) fail_backend_connection(connection, error);
        else {
            connection->phase = SEND_BACKEND_REQUEST;
            connection->stage_deadline = monotonic_milliseconds() + PROXY_REQUEST_SECONDS * 1000;
        }
    }
    /* HUP/RDHUP에서도 남은 데이터를 recv로 확인한다. 클라이언트 SHUT_WR은 응답 포기가 아니다. */
    (void)events;
    if (!connection->closing) advance_connection(connection);
}

/* 헤더/이전 조각을 모두 보낸 뒤에만 producer를 재사용한다. 역압으로 멈췄던 idle 기한도 재시작한다. */
static void resume_backend_body(struct event_connection *connection) {
    free(connection->backend_headers);
    connection->backend_headers = NULL;
    connection->response.wire_header = (struct http_slice){0};
    connection->phase = READ_BACKEND_BODY;
    connection->backend_idle_deadline = monotonic_milliseconds() + PROXY_BODY_IDLE_SECONDS * 1000;
}

/* 출력 전체가 끝난 시점의 단계 전환이다. 부분 송신 중에는 이 함수를 호출하지 않는다. */
static void continue_after_client_output(struct event_connection *connection,
                                          enum connection_phase completed_phase) {
    if (completed_phase == SEND_CONTINUE) {
        connection->phase = READ_CLIENT_BODY;
        connection->client_idle_deadline = monotonic_milliseconds() + IO_CALL_TIMEOUT_SECONDS * 1000;
        return;
    }
    bool body_finished = completed_phase == SEND_CLIENT_BODY && connection->output_is_last;
    bool head_finished = completed_phase == SEND_CLIENT_HEADER && connection->head_request;
    if (body_finished || head_finished) {
        begin_client_drain(connection);
        return;
    }
    if (connection->stream) resume_backend_body(connection);
    else request_next_local_body(connection);
}

/* 송신의 결과 판정과 송신 후 전환을 분리한다. 반환 true이면 다음 step을 즉시 시도할 수 있다. */
static bool advance_client_output(struct event_connection *connection) {
    enum connection_phase completed_phase = connection->phase;
    enum send_result result = send_pending_output(connection, &connection->client);
    if (result == SEND_FAILED) {
        close_event_connection(connection);
        return false;
    }
    if (result == SEND_FINISHED) continue_after_client_output(connection, completed_phase);
    return result != SEND_WOULD_BLOCK;
}

/* 현재 단계가 어떤 기능으로 이어지는지만 보여준다. 대기 상태는 소켓/보조 작업 알림으로 재개한다. */
static bool advance_current_phase(struct event_connection *connection) {
    switch (connection->phase) {
        case READ_CLIENT_HEADER:
            return read_client_request_headers(connection) != 0;
        case READ_CLIENT_BODY:
            return read_client_request_body(connection) != 0;
        case SEND_BACKEND_REQUEST: case READ_BACKEND_HEADER: case READ_BACKEND_BODY: {
            int result = advance_backend_connection(connection);
            if (result < 0) {
                fail_backend_connection(connection, errno);
                return true;
            }
            return result != 0;
        }
        case SEND_CONTINUE: case SEND_CLIENT_HEADER: case SEND_CLIENT_BODY:
            return advance_client_output(connection);
        case DRAIN_CLIENT:
            return drain_client_input(connection) != 0;
        default:
            return false;
    }
}

/* EAGAIN과 budget 소진 모두 같은 방식으로 다음 대기에 필요한 소켓 감시/기한을 갱신한다. */
static void update_connection_wait(struct event_connection *connection) {
    refresh_connection_interest(connection);
    update_connection_timer(connection);
}

/* 상태 머신의 조율: 종료/기한 확인 → 현재 단계 진행 → 다음 이벤트 대기 또는 공정하게 재예약.
 * 함수가 반환되어도 connection은 남아 있으며 다음 이벤트에서 같은 객체로 재개한다. */
void advance_connection(struct event_connection *connection) {
    for (size_t step = 0; step < CONNECTION_STEP_BUDGET; ++step) {
        if (connection->closing || connection->retired) return;
        int64_t deadline = connection_next_deadline(connection);
        if (deadline >= 0 && monotonic_milliseconds() >= deadline) {
            expire_connection(connection);
            return;
        }
        bool can_continue = advance_current_phase(connection);
        if (!can_continue || connection->closing) {
            update_connection_wait(connection);
            return;
        }
    }
    update_connection_wait(connection);
    schedule_connection(connection);
}
