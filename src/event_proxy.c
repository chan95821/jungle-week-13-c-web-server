#define _GNU_SOURCE
#include "event_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* 이벤트 프록시 흐름: start_backend_request → 접속 완료 확인(event_connection.c)
 * → advance_backend_connection에서 요청 송신/응답 헤더/본문 수신을 진행한다.
 * 클라이언트 송신은 event_connection.c가 담당하며, 한 조각의 전송이 끝난 뒤 다시 수신한다. */

/* 공유 순번을 읽고 주소를 복사하는 구간만 잠근다. 네트워크 접속은 이 함수 밖에서 진행한다. */
static int select_backend_address(struct event_connection *connection) {
    struct event_runtime *runtime = connection->worker->runtime;
    /* 이 lock 안에서는 순번 선택만 한다. connect/send/recv 중에는 잡고 있지 않는다. */
    pthread_mutex_lock(&runtime->selection_mutex);
    const struct sockaddr_in *selected = proxy_select_backend(
        runtime->config, &runtime->selection, connection->route.proxy_route);
    if (selected) connection->backend_address = *selected;
    pthread_mutex_unlock(&runtime->selection_mutex);
    if (!selected) { errno = EINVAL; return -1; }
    return 0;
}

/* 프록시 상태와 송신 헤더를 준비한다. 할당 중 실패한 자원도 호출자의 공통 실패 경로가 회수한다. */
static int prepare_backend_request_buffers(struct event_connection *connection) {
    connection->stream = calloc(1, sizeof(*connection->stream));
    if (!connection->stream) return -1;
    connection->stream->backend_fd = -1;
    connection->backend_headers = calloc(1, sizeof(*connection->backend_headers));
    connection->backend_request_header = malloc(PROXY_OUTPUT_HEADER_BYTES);
    if (!connection->backend_headers || !connection->backend_request_header) return -1;
    size_t length;
    /* 기존 HTTP 헤더 정책을 재사용한다. 본문은 복사해서 합치지 않고 별도 출력 조각으로 빌린다. */
    if (build_backend_request_header(&connection->backend_address, &connection->request,
            connection->backend_request_header, PROXY_OUTPUT_HEADER_BYTES, &length) < 0) return -1;
    set_connection_output(connection,
        (struct http_slice){connection->backend_request_header, length}, connection->request.body,
        (struct http_slice){0});
    return 0;
}

/* client와 다른 FD로 접속한다. 즉시 성공하면 송신, EINPROGRESS면 SO_ERROR 확인 이벤트를 기다린다. */
static int connect_selected_backend(struct event_connection *connection) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    connection->backend.fd = fd;
    connection->stream->backend_fd = fd;
    connection->phase = CONNECT_BACKEND;
    connection->stage_deadline = monotonic_milliseconds() + PROXY_CONNECT_SECONDS * 1000;
    /* 즉시 성공이면 송신 단계, EINPROGRESS이면 연결 대기를 유지한다.
     * 이후 epoll 이벤트의 SO_ERROR 확인은 handle_connection_event가 담당한다. */
    if (connect(fd, (const struct sockaddr *)&connection->backend_address,
                 sizeof(connection->backend_address)) == 0) {
        connection->phase = SEND_BACKEND_REQUEST;
        connection->stage_deadline = monotonic_milliseconds() + PROXY_REQUEST_SECONDS * 1000;
    } else if (errno != EINPROGRESS) return -1;
    return 0;
}

/* 요청 전체가 검증된 뒤 주소 선택 → 요청 준비 → 접속을 시작한다. 성공은 통신 전체 완료가 아니다. */
int start_backend_request(struct event_connection *connection) {
    if (select_backend_address(connection) < 0) return -1;
    if (prepare_backend_request_buffers(connection) < 0) return -1;
    return connect_selected_backend(connection);
}

/* 응답 전 실패는 프록시 자원을 회수하고 500(할당)/504(기한)/502(그 외)를 준비한다.
 * 응답이 이미 시작됐으면 다른 상태 줄을 붙이지 않고 닫아 불완전 응답으로 남긴다. */
void fail_backend_connection(struct event_connection *connection, int error) {
    if (connection->response_started) { close_event_connection(connection); return; }
    /* 감시/FD를 먼저 정리하고 헤더·청크 작업 공간을 해제한다. client FD는 오류 전송에 사용한다. */
    detach_endpoint(connection, &connection->backend);
    if (connection->stream) cleanup_backend_response(connection->stream);
    connection->stream = NULL;
    free(connection->backend_headers);
    connection->backend_headers = NULL;
    free(connection->backend_request_header);
    connection->backend_request_header = NULL;
    free(connection->chunk_line);
    connection->chunk_line = NULL;
    free(connection->trailer_bytes);
    connection->trailer_bytes = NULL;
    int status = 502;
    if (error == ENOMEM) status = 500;
    else if (error == ETIMEDOUT) status = 504;
    prepare_connection_error(connection, status);
}

/* 1=data, 0=EAGAIN, 2=EOF, -1=error. 기존 blocking helper와 결과 계약이 다르다. */
/* 아직 소비하지 않은 수신 바이트가 있으면 재사용하고, 없을 때만 nonblocking recv한다.
 * 3은 EINTR로 다음 단계 budget 안에서 재시도하라는 뜻이다. */
static int receive_backend_bytes(struct event_connection *connection) {
    struct proxy_stream *stream = connection->stream;
    /* 헤더와 본문, 여러 청크가 한 recv에 함께 도착할 수 있어 남은 바이트를 버리면 안 된다. */
    if (stream->buffer_offset < stream->buffer_length) return 1;
    ssize_t count = recv(connection->backend.fd, stream->buffer, sizeof(stream->buffer), 0);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 3; /* 다시 시도하되 상위 budget에 포함한다. */
        return -1;
    }
    if (!count) return 2;
    stream->buffer_offset = 0;
    stream->buffer_length = (size_t)count;
    /* 실제 새 바이트를 받았을 때만 idle 기한을 갱신한다. 본문 전체 기한은 연장하지 않는다. */
    connection->backend_idle_deadline = monotonic_milliseconds() + PROXY_BODY_IDLE_SECONDS * 1000;
    return 1;
}

/* 검증된 백엔드 헤더로 클라이언트 응답 상태를 만든다. workspace는 헤더 송신 완료까지 유지한다. */
static void prepare_client_response_from_backend(struct event_connection *connection) {
    struct proxy_stream *stream = connection->stream;
    struct proxy_response_headers *headers = connection->backend_headers;
    stream->remaining = stream->content_length;
    connection->chunk_phase = READ_CHUNK_SIZE;
    connection->body_deadline = monotonic_milliseconds() + PROXY_BODY_SECONDS * 1000;
    connection->response = (struct router_response){
        .status = stream->status,
        .length = stream->framing == BACKEND_LENGTH ? stream->content_length : 0,
        .chunked = stream->framing == BACKEND_CHUNKED || stream->framing == BACKEND_CLOSE,
        .wire_header = {headers->client_header, headers->client_header_length}
    };
    start_client_response(connection);
}

/* 완성 헤더 블록만 파서로 넘긴다. 중간 응답은 보관하고 최종 헤더 검증 후 송신을 준비한다. */
static int read_backend_response_headers(struct event_connection *connection) {
    if (connection->backend_headers->raw_header_length == REQ_HEADER_MAX_BYTES) {
        errno = EPROTO; return -1;
    }
    int available = receive_backend_bytes(connection);
    if (available == 2) { errno = EPROTO; return -1; }
    if (available != 1) return available == 3 ? 1 : available;
    struct proxy_stream *stream = connection->stream;
    struct proxy_response_headers *headers = connection->backend_headers;
    while (stream->buffer_offset < stream->buffer_length) {
        if (headers->raw_header_length == sizeof(headers->raw_header)) { errno = EPROTO; return -1; }
        headers->raw_header[headers->raw_header_length++] = stream->buffer[stream->buffer_offset++];
        size_t length = headers->raw_header_length;
        if (length < 4 || memcmp(headers->raw_header + length - 4, "\r\n\r\n", 4)) continue;
        /* TCP recv의 끝이 HTTP 헤더의 끝은 아니다. CRLFCRLF가 완성된 블록만 파서에 넘긴다. */
        int final = consume_backend_header_block(stream, headers);
        if (final < 0) return -1;
        if (!final) { headers->raw_header_length = 0; continue; }
        prepare_client_response_from_backend(connection);
        return 1;
    }
    return 1;
}

/* 줄의 일부가 도착하면 connection->chunk_line에 남긴다. 스택 지역 배열로는
 * EAGAIN 이후 재개할 수 없다. CRLF도 두 번의 recv에 나뉘어 올 수 있다. */
static int read_backend_chunk_line(struct event_connection *connection) {
    /* chunk 크기 줄과 trailer 줄이 같은 버퍼를 번갈아 사용한다. CRLF까지 누적해서 반환한다. */
    if (!connection->chunk_line) {
        connection->chunk_line = malloc(REQ_HEADER_MAX_BYTES);
        if (!connection->chunk_line) return -1;
    }
    if (connection->chunk_line_length == REQ_HEADER_MAX_BYTES) { errno = EPROTO; return -1; }
    struct proxy_stream *stream = connection->stream;
    int available = receive_backend_bytes(connection);
    if (available == 2) { errno = EPROTO; return -1; }
    if (available != 1) return available == 3 ? 3 : available;
    while (stream->buffer_offset < stream->buffer_length) {
        if (connection->chunk_line_length == REQ_HEADER_MAX_BYTES) { errno = EPROTO; return -1; }
        size_t length = ++connection->chunk_line_length;
        connection->chunk_line[length - 1] = stream->buffer[stream->buffer_offset++];
        if (length >= 2 && !memcmp(connection->chunk_line + length - 2, "\r\n", 2)) return 1;
    }
    return 3; /* 새 데이터는 처리했으나 아직 줄 끝이 아니다. */
}

/* zero chunk 뒤 trailer를 한 줄씩 검증·보관한다. 빈 줄까지 정상 도착해야 마지막 chunk를 만든다.
 * 금지 필드나 누적 8KiB 상한 위반은 성공 종료를 보내지 않고 스트림을 중단하게 한다. */
static int read_chunk_trailers(struct event_connection *connection) {
    int result = read_backend_chunk_line(connection);
    if (result != 1) return result == 3 ? 1 : result;
    size_t length = connection->chunk_line_length;
    connection->chunk_line_length = 0;
    /* CRLF 하나뿐인 빈 줄은 trailer 전체의 끝이다. */
    if (length == 2) {
        connection->stream->trailers = (struct http_slice){connection->trailer_bytes, connection->trailer_length};
        connection->stream->body_complete = true;
        finish_client_body(connection);
        return 1;
    }
    struct http_field field;
    size_t offset = 0;
    if (http_next_field((struct http_slice){connection->chunk_line, length}, &offset, &field) != 1 ||
        !backend_trailer_is_allowed(connection->stream, field)) { errno = EPROTO; return -1; }
    if (!connection->trailer_bytes) {
        connection->trailer_bytes = malloc(REQ_HEADER_MAX_BYTES);
        if (!connection->trailer_bytes) return -1;
    }
    if (proxy_append_bytes(connection->trailer_bytes, REQ_HEADER_MAX_BYTES,
            &connection->trailer_length, connection->chunk_line, length) < 0) return -1;
    return 1;
}

/* chunk 데이터 바로 뒤의 CRLF 두 바이트를 확인한다. 두 바이트가 다른 recv에 와도 위치를 유지한다. */
static int read_chunk_boundary(struct event_connection *connection) {
    struct proxy_stream *stream = connection->stream;
    int available = receive_backend_bytes(connection);
    if (available == 2) { errno = EPROTO; return -1; }
    if (available != 1) return available == 3 ? 1 : available;
    while (connection->chunk_crlf_received < 2 && stream->buffer_offset < stream->buffer_length) {
        unsigned char expected = connection->chunk_crlf_received ? '\n' : '\r';
        if (stream->buffer[stream->buffer_offset++] != expected) { errno = EPROTO; return -1; }
        ++connection->chunk_crlf_received;
    }
    if (connection->chunk_crlf_received == 2) {
        connection->chunk_crlf_received = 0;
        connection->chunk_phase = READ_CHUNK_SIZE;
    }
    return 1;
}

/* 길이/청크 범위 안의 실제 데이터만 송신 조각으로 넘긴다. 조기 EOF는 공통 실패 경로로 전달한다. */
static int read_backend_payload(struct event_connection *connection) {
    struct proxy_stream *stream = connection->stream;
    int available = receive_backend_bytes(connection);
    /* EOF가 정상 본문 끝인 것은 연결 종료 프레이밍일 때뿐이다. 고정 길이/청크 중간 EOF는 오류다. */
    if (available == 2) {
        if (stream->framing != BACKEND_CLOSE) { errno = EPROTO; return -1; }
        stream->body_complete = true;
        finish_client_body(connection);
        return 1;
    }
    if (available != 1) return available == 3 ? 1 : available;
    if (stream->status == 205) { errno = EPROTO; return -1; }
    size_t count = stream->buffer_length - stream->buffer_offset;
    if (stream->framing == BACKEND_LENGTH && count > stream->remaining) count = stream->remaining;
    if (stream->framing == BACKEND_CHUNKED && count > stream->chunk_remaining) count = stream->chunk_remaining;
    struct http_slice chunk = {stream->buffer + stream->buffer_offset, count};
    stream->buffer_offset += count;
    if (stream->framing == BACKEND_LENGTH) stream->remaining -= count;
    if (stream->framing == BACKEND_CHUNKED) stream->chunk_remaining -= count;
    /* 이 borrowed chunk의 전송이 끝날 때까지 phase=SEND_CLIENT_BODY이므로 recv하지 않는다. */
    prepare_client_body_chunk(connection, chunk);
    return 1;
}

/* 크기 줄의 CRLF까지 도착했을 때만 파싱하고 다음 단계(데이터 또는 trailer)를 고른다. */
static int read_next_chunk_size(struct event_connection *connection) {
    int result = read_backend_chunk_line(connection);
    if (result != 1) return result == 3 ? 1 : result;
    size_t length = connection->chunk_line_length - 2;
    connection->chunk_line_length = 0;
    if (parse_chunk_size((struct http_slice){connection->chunk_line, length},
                        &connection->stream->chunk_remaining) < 0) return -1;
    connection->chunk_phase = connection->stream->chunk_remaining ? READ_CHUNK_DATA : READ_CHUNK_TRAILERS;
    return 1;
}

/* chunked의 메타데이터 단계와 실제 데이터 수신을 구분한다. 조각 전송 완료 전에는 재진입하지 않는다. */
static int read_chunked_body(struct event_connection *connection) {
    if (connection->chunk_phase == READ_CHUNK_TRAILERS) return read_chunk_trailers(connection);
    if (connection->chunk_phase == READ_CHUNK_CRLF) return read_chunk_boundary(connection);
    if (connection->chunk_phase == READ_CHUNK_SIZE) return read_next_chunk_size(connection);
    if (!connection->stream->chunk_remaining) {
        connection->chunk_phase = READ_CHUNK_CRLF;
        return 1;
    }
    return read_backend_payload(connection);
}

/* 본문 없음/길이 완료 → 종료, chunked → 청크 단계 처리, 나머지 → 데이터 수신으로 연결한다. */
static int read_backend_response_body(struct event_connection *connection) {
    struct proxy_stream *stream = connection->stream;
    if (stream->framing == BACKEND_NO_BODY ||
        (stream->framing == BACKEND_LENGTH && !stream->remaining)) {
        finish_client_body(connection);
        return 1;
    }
    if (stream->framing == BACKEND_CHUNKED) return read_chunked_body(connection);
    return read_backend_payload(connection);
}

/* advance_connection이 백엔드 I/O 단계에서 호출하는 분기 함수다.
 * 1=현재/다음 단계 재진입 가능, 0=준비 이벤트 대기, -1=errno 오류를 호출자에게 전달한다. */
int advance_backend_connection(struct event_connection *connection) {
    if (connection->phase == SEND_BACKEND_REQUEST) {
        enum send_result result = send_pending_output(connection, &connection->backend);
        if (result == SEND_FAILED) return -1;
        if (result == SEND_WOULD_BLOCK) return 0;
        if (result == SEND_CAN_CONTINUE) return 1;
        /* 헤더+본문이 전부 보내졌으므로 빌린 출력 참조를 비우고 헤더 응답 대기로 넘어간다. */
        free(connection->backend_request_header);
        connection->backend_request_header = NULL;
        connection->output = (struct pending_output){0};
        connection->phase = READ_BACKEND_HEADER;
        connection->stage_deadline = monotonic_milliseconds() + PROXY_HEADER_SECONDS * 1000;
        return 1;
    }
    if (connection->phase == READ_BACKEND_HEADER) return read_backend_response_headers(connection);
    return read_backend_response_body(connection);
}
