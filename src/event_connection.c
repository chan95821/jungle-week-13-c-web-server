#define _GNU_SOURCE
#include "event_internal.h"
#include "handlers/handlers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void dispatch_complete_request(struct event_connection *connection);
static void request_next_local_body(struct event_connection *connection);

struct event_connection *create_event_connection(struct event_worker *worker,
                                                  struct accepted_client accepted) {
    struct event_connection *connection = calloc(1, sizeof(*connection));
    if (!connection) return NULL;
    connection->worker = worker;
    connection->client = (struct event_endpoint){connection, accepted.fd, 0, false};
    connection->backend = (struct event_endpoint){connection, -1, 0, true};
    connection->phase = READ_CLIENT_HEADER;
    connection->heap_index = SIZE_MAX;
    connection->stage_deadline = accepted.accepted_at + HEADER_DEADLINE_SECONDS * 1000;
    connection->next = worker->connections;
    if (worker->connections) worker->connections->previous = connection;
    worker->connections = connection;
    schedule_connection(connection);
    return connection;
}

void detach_endpoint(struct event_connection *connection, struct event_endpoint *endpoint) {
    if (endpoint->fd < 0) return;
    if (endpoint->interest)
        epoll_ctl(connection->worker->epoll_fd, EPOLL_CTL_DEL, endpoint->fd, NULL);
    close(endpoint->fd); /* Linux close는 EINTR이어도 재시도하지 않는다. */
    endpoint->fd = -1;
    endpoint->interest = 0;
    if (endpoint->backend && connection->stream) connection->stream->backend_fd = -1;
}

void close_event_connection(struct event_connection *connection) {
    if (connection->retired) return;
    connection->closing = true;
    forget_connection_timer(connection);
    detach_endpoint(connection, &connection->client);
    detach_endpoint(connection, &connection->backend);
    /* 보조 스레드가 request/response를 만지고 있으면 메모리/상태를 건드리지 않는다. */
    if (connection->local_job_inflight) return;
    if (connection->response.body_provider.cleanup_func && !connection->local_cleanup_done) {
        connection->phase = WAIT_LOCAL_CLEANUP;
        submit_local_job(connection, CLEANUP_LOCAL_RESPONSE);
        return;
    }
    retire_connection(connection);
}

void release_event_connection(struct event_connection *connection) {
    if (connection->stream) cleanup_backend_response(connection->stream);
    free(connection->backend_headers);
    free(connection->backend_request_header);
    free(connection->chunk_line);
    free(connection->trailer_bytes);
    free(connection->request_body);
    free(connection);
}

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

int64_t connection_next_deadline(const struct event_connection *connection) {
    if (connection->closing || connection->retired) return -1;
    switch (connection->phase) {
        case READ_CLIENT_HEADER: case READ_CLIENT_BODY: case CONNECT_BACKEND:
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

int send_pending_output(struct event_connection *connection, struct event_endpoint *endpoint) {
    struct pending_output *output = &connection->output;
    if (output->index == output->count) return 1;
    struct iovec parts[3];
    size_t count = output->count - output->index;
    for (size_t i = 0; i < count; ++i) parts[i] = output->parts[output->index + i];
    parts[0].iov_base = (unsigned char *)parts[0].iov_base + output->offset;
    parts[0].iov_len -= output->offset;
    struct msghdr message = {.msg_iov = parts, .msg_iovlen = count};
    ssize_t sent = sendmsg(endpoint->fd, &message, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 2;
        return -1;
    }
    if (sent == 0) { errno = EIO; return -1; }
    if (!endpoint->backend && connection->phase != SEND_CONTINUE)
        connection->response_started = true;
    connection->write_deadline = monotonic_milliseconds() + IO_CALL_TIMEOUT_SECONDS * 1000;
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
    return output->index == output->count ? 1 : 2;
}

void prepare_connection_error(struct event_connection *connection, int status) {
    if (connection->response_started) { close_event_connection(connection); return; }
    router_prepare_error(&connection->response, status, connection->allow);
    start_client_response(connection);
}

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

void begin_client_drain(struct event_connection *connection) {
    detach_endpoint(connection, &connection->backend);
    if (shutdown(connection->client.fd, SHUT_WR) < 0) {
        close_event_connection(connection); return;
    }
    connection->phase = DRAIN_CLIENT;
    connection->stage_deadline = monotonic_milliseconds() + DRAIN_DEADLINE_SECONDS * 1000;
}

static void process_received_request_header(struct event_connection *connection) {
    connection->stage_deadline = monotonic_milliseconds() + BODY_DEADLINE_SECONDS * 1000;
    int status = http_parse_request(connection->request_header, connection->header_end, &connection->request);
    connection->head_request = http_slice_equal(connection->request.method, "HEAD");
    if (!status) status = http_select_route(&connection->request, &connection->route, &connection->allow);
    if (!status && connection->route.proxy_route != PROXY_ROUTE_LOCAL)
        status = proxy_validate_request(&connection->request);
    if (status) { prepare_connection_error(connection, status); return; }
    if (connection->request.content_length) {
        connection->request_body = malloc(connection->request.content_length);
        if (!connection->request_body) { prepare_connection_error(connection, 500); return; }
        size_t received = connection->header_received - connection->header_end;
        if (received > connection->request.content_length) received = connection->request.content_length;
        memcpy(connection->request_body, connection->request_header + connection->header_end, received);
        connection->body_received = received;
    }
    connection->phase = READ_CLIENT_BODY;
    if (connection->request.expect_continue &&
        connection->body_received < connection->request.content_length) {
        connection->phase = SEND_CONTINUE;
        set_connection_output(connection,
            (struct http_slice){(const unsigned char *)"HTTP/1.1 100 Continue\r\n\r\n", 25},
            (struct http_slice){0}, (struct http_slice){0});
    }
}

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
    return 1;
}

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

static void consume_local_body_result(struct event_connection *connection,
                                      enum body_result result, struct http_slice chunk) {
    if (result == BODY_DATA) prepare_client_body_chunk(connection, chunk);
    else if (result == BODY_END) finish_client_body(connection);
    else close_event_connection(connection);
}

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

void handle_connection_event(struct event_endpoint *endpoint, uint32_t events) {
    struct event_connection *connection = endpoint->connection;
    if (connection->closing || connection->retired || endpoint->fd < 0) return;
    if (endpoint->backend && connection->phase == CONNECT_BACKEND) {
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

void advance_connection(struct event_connection *connection) {
    for (size_t step = 0; step < CONNECTION_STEP_BUDGET; ++step) {
        if (connection->closing || connection->retired) return;
        int64_t deadline = connection_next_deadline(connection);
        if (deadline >= 0 && monotonic_milliseconds() >= deadline) {
            expire_connection(connection); return;
        }
        int progressed = 0;
        switch (connection->phase) {
            case READ_CLIENT_HEADER: progressed = read_client_request_headers(connection); break;
            case READ_CLIENT_BODY: progressed = read_client_request_body(connection); break;
            case CONNECT_BACKEND: break; /* EPOLLOUT 후 SO_ERROR 확인 전에는 연결 성공이 아니다. */
            case SEND_BACKEND_REQUEST: case READ_BACKEND_HEADER: case READ_BACKEND_BODY:
                progressed = advance_backend_connection(connection);
                if (progressed < 0) { fail_backend_connection(connection, errno); progressed = 1; }
                break;
            case SEND_CONTINUE: case SEND_CLIENT_HEADER: case SEND_CLIENT_BODY: {
                enum connection_phase phase = connection->phase;
                int sent = send_pending_output(connection, &connection->client);
                if (sent < 0) { close_event_connection(connection); break; }
                progressed = sent != 0;
                if (sent != 1) break;
                if (phase == SEND_CONTINUE) connection->phase = READ_CLIENT_BODY;
                else if (phase == SEND_CLIENT_BODY && connection->output_is_last)
                    begin_client_drain(connection);
                else if (phase == SEND_CLIENT_HEADER && connection->head_request)
                    begin_client_drain(connection);
                else if (connection->stream) {
                    /* wire header는 모두 전송했다. 이후 proxy parser가 버퍼를 재사용해도 안전하다. */
                    free(connection->backend_headers);
                    connection->backend_headers = NULL;
                    connection->response.wire_header = (struct http_slice){0};
                    connection->phase = READ_BACKEND_BODY;
                    connection->backend_idle_deadline = monotonic_milliseconds() + PROXY_BODY_IDLE_SECONDS * 1000;
                } else request_next_local_body(connection);
                break;
            }
            case DRAIN_CLIENT: progressed = drain_client_input(connection); break;
            default: break;
        }
        if (!progressed || connection->closing) {
            refresh_connection_interest(connection);
            update_connection_timer(connection);
            return;
        }
    }
    /* budget 소진은 오류가 아니다. 다른 연결에 기회를 주고 같은 연결을 이어 처리한다. */
    refresh_connection_interest(connection);
    update_connection_timer(connection);
    schedule_connection(connection);
}
