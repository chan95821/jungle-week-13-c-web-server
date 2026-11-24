#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "socket_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

enum backend_framing { BACKEND_LENGTH, BACKEND_CHUNKED, BACKEND_CLOSE, BACKEND_NO_BODY };

struct proxy_stream {
    int backend_fd;
    const volatile sig_atomic_t *running;
    struct timespec header_deadline;
    struct timespec body_deadline;
    bool body_started;
    unsigned char buffer[PROXY_STREAM_BYTES];
    size_t buffer_offset, buffer_length;
    unsigned char raw_header[REQ_HEADER_MAX_BYTES];
    size_t raw_header_length;
    unsigned char client_header[2 * REQ_HEADER_MAX_BYTES + 256];
    size_t client_header_length;
    unsigned char trailer_buffer[REQ_HEADER_MAX_BYTES];
    struct http_slice trailers;
    struct http_slice fields;
    struct http_slice status_line;
    enum backend_framing framing;
    size_t content_length;
    bool has_content_length;
    size_t remaining;
    size_t chunk_remaining;
    bool chunk_needs_crlf;
    bool body_complete;
    int status;
};

static int connect_backend(struct proxy_stream *, const struct sockaddr_in *);
static int forward_request_to_backend(struct proxy_stream *, const struct sockaddr_in *,
                                      const struct http_request *);
static int receive_backend_response_headers(struct proxy_stream *);
static int build_client_response_headers(struct proxy_stream *);
static enum body_result read_backend_body_chunk(void *, struct http_slice *);
static void cleanup_backend_response(void *);
static int make_deadline(struct timespec *, int);
static bool deadline_reached(const struct timespec *);
static int fill_backend_buffer(struct proxy_stream *);
static int read_backend_byte(struct proxy_stream *, unsigned char *);
static int read_backend_line(struct proxy_stream *, unsigned char *, size_t, size_t *);
static int parse_backend_headers(struct proxy_stream *);
static bool remove_connection_header(struct http_slice, struct http_slice);
static int validate_connection_value(struct http_slice);
static int append_bytes(unsigned char *, size_t, size_t *, const void *, size_t);
static int append_format(unsigned char *, size_t, size_t *, const char *, ...);
static int parse_decimal_length(struct http_slice, size_t *);
static bool token_byte(unsigned char);
static int parse_chunk_size(struct http_slice, size_t *);
static int read_trailers(struct proxy_stream *);

/* Request lifecycle: connect -> forward -> validate headers -> transfer ownership. */
void proxy_prepare_response(const struct sockaddr_in *backend,
                            const struct http_request *request,
                            const volatile sig_atomic_t *running,
                            struct router_response *response) {
    struct proxy_stream *stream = calloc(1, sizeof(*stream));
    if (!stream) { router_prepare_error(response, 500, NULL); return; }
    stream->backend_fd = -1;
    stream->running = running;
    if (connect_backend(stream, backend) < 0 ||
        forward_request_to_backend(stream, backend, request) < 0 ||
        make_deadline(&stream->header_deadline, PROXY_HEADER_SECONDS) < 0 ||
        receive_backend_response_headers(stream) < 0 ||
        build_client_response_headers(stream) < 0 ||
        make_deadline(&stream->body_deadline, PROXY_BODY_SECONDS) < 0) {
        int saved_errno = errno;
        cleanup_backend_response(stream);
        int status = saved_errno == ENOMEM ? 500 :
                     saved_errno == ETIMEDOUT ? 504 : 502;
        router_prepare_error(response, status, NULL);
        return;
    }
    stream->body_started = true;
    stream->remaining = stream->content_length;
    *response = (struct router_response){
        .status = stream->status,
        .length = stream->framing == BACKEND_LENGTH ? stream->content_length : 0,
        .chunked = stream->framing == BACKEND_CHUNKED || stream->framing == BACKEND_CLOSE,
        .wire_header = {stream->client_header, stream->client_header_length},
        .body_provider = {
            .next_func = read_backend_body_chunk,
            .cleanup_func = cleanup_backend_response,
            .state = stream,
            .trailers = &stream->trailers
        }
    };
}

/* A bounded non-blocking connect is local to this sequential request, not an event loop. */
static int connect_backend(struct proxy_stream *stream, const struct sockaddr_in *backend) {
    struct timespec deadline;
    if (make_deadline(&deadline, PROXY_CONNECT_SECONDS) < 0) return -1;
    stream->backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (stream->backend_fd < 0) return -1;
    int fd = stream->backend_fd;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || set_close_on_exec(fd) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    if (connect(fd, (const struct sockaddr *)backend, sizeof(*backend)) < 0) {
        if (errno != EINPROGRESS && errno != EINTR) return -1;
        for (;;) {
            if (stream->running && !*stream->running) { errno = ECANCELED; return -1; }
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
            long long milliseconds = (deadline.tv_sec - now.tv_sec) * 1000LL +
                (deadline.tv_nsec - now.tv_nsec + 999999LL) / 1000000LL;
            if (milliseconds <= 0) { errno = ETIMEDOUT; return -1; }
            struct pollfd ready = {.fd = fd, .events = POLLOUT};
            int result = poll(&ready, 1, (int)milliseconds);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0) return -1;
            if (result == 0) { errno = ETIMEDOUT; return -1; }
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return -1;
            if (error) { errno = error; return -1; }
            break;
        }
    }
    if (deadline_reached(&deadline)) { errno = ETIMEDOUT; return -1; }
    return fcntl(fd, F_SETFL, flags);
}

static int forward_request_to_backend(struct proxy_stream *stream,
                                      const struct sockaddr_in *backend,
                                      const struct http_request *request) {
    unsigned char header[2 * REQ_HEADER_MAX_BYTES + 256];
    size_t used = 0;
    char address[INET_ADDRSTRLEN];
    struct timespec deadline;
    if (!inet_ntop(AF_INET, &backend->sin_addr, address, sizeof(address)) ||
        make_deadline(&deadline, PROXY_REQUEST_SECONDS) < 0) return -1;
    if (append_format(header, sizeof(header), &used, "%.*s %.*s HTTP/1.1\r\n",
                      (int)request->method.length, request->method.data,
                      (int)request->target.length, request->target.data) < 0) return -1;
    size_t offset = 0;
    struct http_field field;
    int next;
    while ((next = http_next_field(request->headers, &offset, &field)) > 0) {
        if (remove_connection_header(request->headers, field.name) ||
            http_field_name_equal(field.name, "host") ||
            http_field_name_equal(field.name, "content-length") ||
            http_field_name_equal(field.name, "expect") ||
            http_field_name_equal(field.name, "trailer")) continue;
        if (append_format(header, sizeof(header), &used, "%.*s: %.*s\r\n",
                          (int)field.name.length, field.name.data,
                          (int)field.value.length, field.value.data) < 0) return -1;
    }
    if (next < 0) { errno = EPROTO; return -1; }
    if (append_format(header, sizeof(header), &used,
        "Host: %s:%u\r\nContent-Length: %zu\r\nConnection: close\r\n"
        "Via: 1.1 jungle-proxy\r\n\r\n",
        address, (unsigned int)ntohs(backend->sin_port), request->body.length) < 0)
        return -1;
    struct io_context output = {stream->running, &deadline};
    if (send_all_with_deadline(stream->backend_fd, header, used, &output) < 0 ||
        send_all_with_deadline(stream->backend_fd, request->body.data,
                               request->body.length, &output) < 0) {
        if (socket_io_timed_out()) errno = ETIMEDOUT;
        return -1;
    }
    if (deadline_reached(&deadline)) { errno = ETIMEDOUT; return -1; }
    return 0;
}

static int receive_backend_response_headers(struct proxy_stream *stream) {
    do {
        stream->raw_header_length = 0;
        while (stream->raw_header_length < sizeof(stream->raw_header)) {
            unsigned char byte;
            if (read_backend_byte(stream, &byte) < 0) return -1;
            stream->raw_header[stream->raw_header_length++] = byte;
            size_t length = stream->raw_header_length;
            if (length >= 4 && memcmp(stream->raw_header + length - 4, "\r\n\r\n", 4) == 0)
                break;
        }
        size_t length = stream->raw_header_length;
        if (length < 4 || memcmp(stream->raw_header + length - 4, "\r\n\r\n", 4) != 0) {
            errno = EPROTO; return -1;
        }
        if (parse_backend_headers(stream) < 0) return -1;
        if (stream->status == 101) { errno = EPROTO; return -1; }
        /* Preserve informational fields, except 100 already handled at the proxy.
         * Retain them within the bounded output header until final headers validate. */
        if (stream->status > 100 && stream->status < 200 &&
            build_client_response_headers(stream) < 0) return -1;
        if (deadline_reached(&stream->header_deadline)) { errno = ETIMEDOUT; return -1; }
    } while (stream->status < 200);
    return 0;
}

static int build_client_response_headers(struct proxy_stream *stream) {
    size_t *used = &stream->client_header_length;
    unsigned char *header = stream->client_header;
    size_t capacity = sizeof(stream->client_header);
    if (append_bytes(header, capacity, used, stream->status_line.data,
                     stream->status_line.length) < 0 ||
        append_bytes(header, capacity, used, "\r\n", 2) < 0) return -1;
    size_t offset = 0;
    struct http_field field;
    int next;
    while ((next = http_next_field(stream->fields, &offset, &field)) > 0) {
        if (remove_connection_header(stream->fields, field.name) ||
            http_field_name_equal(field.name, "content-length")) continue;
        if (http_field_name_equal(field.name, "trailer") &&
            stream->framing != BACKEND_CHUNKED) continue;
        if (append_format(header, capacity, used, "%.*s: %.*s\r\n",
            (int)field.name.length, field.name.data, (int)field.value.length, field.value.data) < 0)
            return -1;
    }
    if (next < 0) { errno = EPROTO; return -1; }
    if (stream->framing == BACKEND_LENGTH ||
        (stream->status == 304 && stream->has_content_length)) {
        if (append_format(header, capacity, used, "Content-Length: %zu\r\n",
                          stream->content_length) < 0) return -1;
    } else if (stream->framing == BACKEND_CHUNKED || stream->framing == BACKEND_CLOSE) {
        if (append_format(header, capacity, used, "Transfer-Encoding: chunked\r\n") < 0)
            return -1;
    }
    return append_format(header, capacity, used,
                         "Connection: close\r\nVia: 1.1 jungle-proxy\r\n\r\n");
}

static enum body_result read_backend_body_chunk(void *state, struct http_slice *chunk) {
    struct proxy_stream *stream = state;
    if (stream->body_complete || stream->framing == BACKEND_NO_BODY ||
        (stream->framing == BACKEND_LENGTH && stream->remaining == 0)) return BODY_END;
    if (stream->running && !*stream->running) { errno = ECANCELED; return BODY_ERROR; }
    if (deadline_reached(&stream->body_deadline)) { errno = ETIMEDOUT; return BODY_ERROR; }

    if (stream->framing == BACKEND_CHUNKED && stream->chunk_remaining == 0) {
        unsigned char line[REQ_HEADER_MAX_BYTES];
        size_t length;
        if (stream->chunk_needs_crlf) {
            unsigned char cr, lf;
            if (read_backend_byte(stream, &cr) < 0 || read_backend_byte(stream, &lf) < 0)
                return BODY_ERROR;
            if (cr != '\r' || lf != '\n') { errno = EPROTO; return BODY_ERROR; }
        }
        if (read_backend_line(stream, line, sizeof(line), &length) < 0 ||
            parse_chunk_size((struct http_slice){line, length}, &stream->chunk_remaining) < 0)
            return BODY_ERROR;
        stream->chunk_needs_crlf = true;
        if (stream->chunk_remaining == 0) {
            if (read_trailers(stream) < 0) return BODY_ERROR;
            stream->body_complete = true;
            return BODY_END;
        }
    }

    int available = fill_backend_buffer(stream);
    if (available < 0) return BODY_ERROR;
    if (available == 0) {
        if (stream->framing != BACKEND_CLOSE) { errno = EPROTO; return BODY_ERROR; }
        stream->body_complete = true;
        return BODY_END;
    }
    size_t count = stream->buffer_length - stream->buffer_offset;
    if (stream->status == 205 && count) { errno = EPROTO; return BODY_ERROR; }
    if (stream->framing == BACKEND_LENGTH && count > stream->remaining) count = stream->remaining;
    if (stream->framing == BACKEND_CHUNKED && count > stream->chunk_remaining)
        count = stream->chunk_remaining;
    *chunk = (struct http_slice){stream->buffer + stream->buffer_offset, count};
    stream->buffer_offset += count;
    if (stream->framing == BACKEND_LENGTH) stream->remaining -= count;
    if (stream->framing == BACKEND_CHUNKED) stream->chunk_remaining -= count;
    return BODY_DATA;
}

static void cleanup_backend_response(void *state) {
    struct proxy_stream *stream = state;
    if (stream->backend_fd >= 0) close(stream->backend_fd);
    free(stream);
}
static int make_deadline(struct timespec *deadline, int seconds) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) return -1;
    deadline->tv_sec += seconds;
    return 0;
}

static bool deadline_reached(const struct timespec *deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return true;
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

/* Refill one reusable buffer. Returned slices stay valid until the next refill. */
static int fill_backend_buffer(struct proxy_stream *stream) {
    if (stream->buffer_offset < stream->buffer_length) return 1;
    struct timespec deadline = stream->header_deadline;
    if (stream->body_started) {
        if (make_deadline(&deadline, PROXY_BODY_IDLE_SECONDS) < 0) return -1;
        if (stream->body_deadline.tv_sec < deadline.tv_sec ||
            (stream->body_deadline.tv_sec == deadline.tv_sec &&
             stream->body_deadline.tv_nsec < deadline.tv_nsec))
            deadline = stream->body_deadline;
    }
    struct io_context input = {stream->running, &deadline};
    ssize_t count = receive_with_deadline(stream->backend_fd, stream->buffer,
                                          sizeof(stream->buffer), &input);
    if (count < 0) {
        if (socket_io_timed_out()) errno = ETIMEDOUT;
        return -1;
    }
    stream->buffer_offset = 0;
    stream->buffer_length = (size_t)count;
    return count != 0;
}

static int read_backend_byte(struct proxy_stream *stream, unsigned char *byte) {
    int available = fill_backend_buffer(stream);
    if (available < 0) return -1;
    if (available == 0) { errno = EPROTO; return -1; }
    *byte = stream->buffer[stream->buffer_offset++];
    return 0;
}

static int read_backend_line(struct proxy_stream *stream, unsigned char *line,
                              size_t capacity, size_t *length) {
    *length = 0;
    while (*length < capacity) {
        unsigned char byte;
        if (read_backend_byte(stream, &byte) < 0) return -1;
        line[(*length)++] = byte;
        if (*length >= 2 && line[*length - 2] == '\r' && byte == '\n') {
            *length -= 2;
            return 0;
        }
    }
    errno = EPROTO;
    return -1;
}

static int parse_backend_headers(struct proxy_stream *stream) {
    size_t status_length = 0;
    while (status_length + 1 < stream->raw_header_length &&
           memcmp(stream->raw_header + status_length, "\r\n", 2) != 0) ++status_length;
    const unsigned char *line = stream->raw_header;
    if (status_length < 13 || memcmp(line, "HTTP/1.1 ", 9) != 0 || line[12] != ' ' ||
        line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9') { errno = EPROTO; return -1; }
    for (size_t i = 13; i < status_length; ++i) {
        if ((line[i] < 32 && line[i] != '\t') || line[i] == 127) {
            errno = EPROTO; return -1;
        }
    }
    stream->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    stream->status_line = (struct http_slice){line, status_length};
    stream->fields = (struct http_slice){
        line + status_length + 2, stream->raw_header_length - status_length - 4
    };
    bool length_seen = false, transfer_seen = false;
    stream->content_length = SIZE_MAX;
    size_t offset = 0;
    struct http_field field;
    int next;
    while ((next = http_next_field(stream->fields, &offset, &field)) > 0) {
        if (http_field_name_equal(field.name, "content-length")) {
            if (length_seen || parse_decimal_length(field.value, &stream->content_length) < 0)
                goto invalid;
            length_seen = true;
        } else if (http_field_name_equal(field.name, "transfer-encoding")) {
            if (transfer_seen || !http_field_name_equal(field.value, "chunked")) goto invalid;
            transfer_seen = true;
        } else if (http_field_name_equal(field.name, "connection") ||
                   http_field_name_equal(field.name, "trailer")) {
            if (validate_connection_value(field.value) < 0) goto invalid;
        }
    }
    if (next < 0 || (length_seen && transfer_seen)) goto invalid;
    if (stream->status == 205 && length_seen && stream->content_length != 0) goto invalid;
    stream->has_content_length = length_seen;
    if (stream->status < 200 || stream->status == 204) {
        if (length_seen || transfer_seen) goto invalid;
        stream->framing = BACKEND_NO_BODY;
    } else if (stream->status == 304) {
        stream->framing = BACKEND_NO_BODY;
    } else {
        stream->framing = transfer_seen ? BACKEND_CHUNKED :
                          length_seen ? BACKEND_LENGTH : BACKEND_CLOSE;
    }
    return 0;
invalid:
    errno = EPROTO;
    return -1;
}

static bool slices_equal_case(struct http_slice a, struct http_slice b) {
    if (a.length != b.length) return false;
    for (size_t i = 0; i < a.length; ++i) {
        unsigned char left = a.data[i], right = b.data[i];
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) return false;
    }
    return true;
}

static bool remove_connection_header(struct http_slice fields, struct http_slice name) {
    static const char *const fixed[] = {
        "connection", "keep-alive", "proxy-connection", "te", "transfer-encoding",
        "upgrade", "proxy-authenticate", "proxy-authorization"
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        if (http_field_name_equal(name, fixed[i])) return true;
    size_t offset = 0;
    struct http_field field;
    while (http_next_field(fields, &offset, &field) > 0) {
        if (!http_field_name_equal(field.name, "connection")) continue;
        size_t position = 0;
        while (position < field.value.length) {
            while (position < field.value.length &&
                   (field.value.data[position] == ' ' || field.value.data[position] == '\t'))
                ++position;
            size_t start = position;
            while (position < field.value.length && token_byte(field.value.data[position]))
                ++position;
            if (slices_equal_case(name, (struct http_slice){
                    field.value.data + start, position - start})) return true;
            while (position < field.value.length && field.value.data[position] != ',') ++position;
            if (position < field.value.length) ++position;
        }
    }
    return false;
}

static int validate_connection_value(struct http_slice value) {
    size_t position = 0;
    for (;;) {
        while (position < value.length &&
               (value.data[position] == ' ' || value.data[position] == '\t')) ++position;
        size_t start = position;
        while (position < value.length && token_byte(value.data[position])) ++position;
        if (start == position) return -1;
        while (position < value.length &&
               (value.data[position] == ' ' || value.data[position] == '\t')) ++position;
        if (position == value.length) return 0;
        if (value.data[position++] != ',') return -1;
    }
}

static int append_bytes(unsigned char *buffer, size_t capacity, size_t *used,
                         const void *bytes, size_t length) {
    if (length > capacity - *used) { errno = EOVERFLOW; return -1; }
    memcpy(buffer + *used, bytes, length);
    *used += length;
    return 0;
}

static int append_format(unsigned char *buffer, size_t capacity, size_t *used,
                          const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf((char *)buffer + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (count < 0 || (size_t)count >= capacity - *used) { errno = EOVERFLOW; return -1; }
    *used += (size_t)count;
    return 0;
}

static int parse_decimal_length(struct http_slice value, size_t *length) {
    if (!value.length) return -1;
    *length = 0;
    for (size_t i = 0; i < value.length; ++i) {
        unsigned char c = value.data[i];
        if (c < '0' || c > '9' || *length > (SIZE_MAX - (c - '0')) / 10) return -1;
        *length = *length * 10 + c - '0';
    }
    return 0;
}

static bool token_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int parse_chunk_size(struct http_slice line, size_t *size) {
    size_t position = 0;
    *size = 0;
    while (position < line.length) {
        unsigned char c = line.data[position];
        unsigned int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        if (*size > (SIZE_MAX - digit) / 16) goto invalid;
        *size = *size * 16 + digit;
        ++position;
    }
    if (position == 0) goto invalid;
    while (position < line.length) {
        while (position < line.length &&
               (line.data[position] == ' ' || line.data[position] == '\t')) ++position;
        if (position == line.length || line.data[position++] != ';') goto invalid;
        while (position < line.length &&
               (line.data[position] == ' ' || line.data[position] == '\t')) ++position;
        size_t start = position;
        while (position < line.length && token_byte(line.data[position])) ++position;
        if (start == position) goto invalid;
        while (position < line.length &&
               (line.data[position] == ' ' || line.data[position] == '\t')) ++position;
        if (position < line.length && line.data[position] == '=') {
            ++position;
            while (position < line.length &&
                   (line.data[position] == ' ' || line.data[position] == '\t')) ++position;
            if (position < line.length && line.data[position] == '"') {
                ++position;
                bool closed = false;
                while (position < line.length) {
                    unsigned char c = line.data[position++];
                    if (c == '"') { closed = true; break; }
                    if (c == '\\') {
                        if (position == line.length) goto invalid;
                        c = line.data[position++];
                    }
                    if ((c < 32 && c != '\t') || c == 127) goto invalid;
                }
                if (!closed) goto invalid;
            } else {
                start = position;
                while (position < line.length && token_byte(line.data[position])) ++position;
                if (start == position) goto invalid;
            }
        }
    }
    return 0;
invalid:
    errno = EPROTO;
    return -1;
}

static int read_trailers(struct proxy_stream *stream) {
    size_t total = 0;
    for (;;) {
        unsigned char line[REQ_HEADER_MAX_BYTES];
        size_t length;
        if (read_backend_line(stream, line, sizeof(line), &length) < 0) return -1;
        if (length == 0) break;
        line[length] = '\r';
        line[length + 1] = '\n';
        struct http_slice fields = {line, length + 2};
        size_t offset = 0;
        struct http_field field;
        if (http_next_field(fields, &offset, &field) != 1 ||
            remove_connection_header(stream->fields, field.name) ||
            http_field_name_equal(field.name, "content-length") ||
            http_field_name_equal(field.name, "host") ||
            http_field_name_equal(field.name, "trailer") ||
            http_field_name_equal(field.name, "content-type") ||
            http_field_name_equal(field.name, "content-encoding") ||
            http_field_name_equal(field.name, "content-range")) {
            errno = EPROTO; return -1;
        }
        if (append_bytes(stream->trailer_buffer, sizeof(stream->trailer_buffer),
                          &total, line, length + 2) < 0) return -1;
    }
    stream->trailers = (struct http_slice){stream->trailer_buffer, total};
    return 0;
}