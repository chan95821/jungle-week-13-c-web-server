#define _GNU_SOURCE
#include "event_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int start_backend_request(struct event_connection *connection) {
    struct event_runtime *runtime = connection->worker->runtime;
    /* 이 lock 안에서는 순번 선택만 한다. connect/send/recv 중에는 잡고 있지 않는다. */
    pthread_mutex_lock(&runtime->selection_mutex);
    const struct sockaddr_in *selected = proxy_select_backend(
        runtime->config, &runtime->selection, connection->route.proxy_route);
    if (selected) connection->backend_address = *selected;
    pthread_mutex_unlock(&runtime->selection_mutex);
    if (!selected) { errno = EINVAL; return -1; }

    connection->stream = calloc(1, sizeof(*connection->stream));
    if (!connection->stream) return -1;
    connection->stream->backend_fd = -1;
    connection->backend_headers = calloc(1, sizeof(*connection->backend_headers));
    connection->backend_request_header = malloc(PROXY_OUTPUT_HEADER_BYTES);
    if (!connection->backend_headers || !connection->backend_request_header) return -1;
    size_t length;
    if (build_backend_request_header(&connection->backend_address, &connection->request,
            connection->backend_request_header, PROXY_OUTPUT_HEADER_BYTES, &length) < 0) return -1;
    set_connection_output(connection,
        (struct http_slice){connection->backend_request_header, length}, connection->request.body,
        (struct http_slice){0});

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    connection->backend.fd = fd;
    connection->stream->backend_fd = fd;
    connection->phase = CONNECT_BACKEND;
    connection->stage_deadline = monotonic_milliseconds() + PROXY_CONNECT_SECONDS * 1000;
    if (connect(fd, (const struct sockaddr *)&connection->backend_address,
                 sizeof(connection->backend_address)) == 0) {
        connection->phase = SEND_BACKEND_REQUEST;
        connection->stage_deadline = monotonic_milliseconds() + PROXY_REQUEST_SECONDS * 1000;
    } else if (errno != EINPROGRESS) return -1;
    return 0;
}

void fail_backend_connection(struct event_connection *connection, int error) {
    if (connection->response_started) { close_event_connection(connection); return; }
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
    prepare_connection_error(connection, error == ENOMEM ? 500 : error == ETIMEDOUT ? 504 : 502);
}

/* 1=data, 0=EAGAIN, 2=EOF, -1=error. 기존 blocking helper와 결과 계약이 다르다. */
static int receive_backend_bytes(struct event_connection *connection) {
    struct proxy_stream *stream = connection->stream;
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
    connection->backend_idle_deadline = monotonic_milliseconds() + PROXY_BODY_IDLE_SECONDS * 1000;
    return 1;
}

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
        int final = consume_backend_header_block(stream, headers);
        if (final < 0) return -1;
        if (!final) { headers->raw_header_length = 0; continue; }
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
        return 1;
    }
    return 1;
}

/* 줄의 일부가 도착하면 connection->chunk_line에 남긴다. 스택 지역 배열로는
 * EAGAIN 이후 재개할 수 없다. CRLF도 두 번의 recv에 나뉘어 올 수 있다. */
static int read_backend_chunk_line(struct event_connection *connection) {
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

static int read_chunk_trailers(struct event_connection *connection) {
    int result = read_backend_chunk_line(connection);
    if (result != 1) return result == 3 ? 1 : result;
    size_t length = connection->chunk_line_length;
    connection->chunk_line_length = 0;
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

static int read_backend_response_body(struct event_connection *connection) {
    struct proxy_stream *stream = connection->stream;
    if (stream->framing == BACKEND_NO_BODY ||
        (stream->framing == BACKEND_LENGTH && !stream->remaining)) {
        finish_client_body(connection);
        return 1;
    }
    if (stream->framing == BACKEND_CHUNKED) {
        if (connection->chunk_phase == READ_CHUNK_TRAILERS) return read_chunk_trailers(connection);
        if (connection->chunk_phase == READ_CHUNK_CRLF) return read_chunk_boundary(connection);
        if (connection->chunk_phase == READ_CHUNK_SIZE) {
            int result = read_backend_chunk_line(connection);
            if (result != 1) return result == 3 ? 1 : result;
            size_t length = connection->chunk_line_length - 2;
            connection->chunk_line_length = 0;
            if (parse_chunk_size((struct http_slice){connection->chunk_line, length},
                                  &stream->chunk_remaining) < 0) return -1;
            connection->chunk_phase = stream->chunk_remaining ? READ_CHUNK_DATA : READ_CHUNK_TRAILERS;
            return 1;
        }
        if (!stream->chunk_remaining) {
            connection->chunk_phase = READ_CHUNK_CRLF;
            return 1;
        }
    }
    int available = receive_backend_bytes(connection);
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

int advance_backend_connection(struct event_connection *connection) {
    if (connection->phase == SEND_BACKEND_REQUEST) {
        int result = send_pending_output(connection, &connection->backend);
        if (result != 1) return result == 2 ? 1 : result;
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
