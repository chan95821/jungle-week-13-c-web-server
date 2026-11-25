#define _POSIX_C_SOURCE 200809L
#include "proxy_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int retain_trailer_connection_fields(struct proxy_stream *, struct http_slice);
static int validate_connection_value(struct http_slice);
static int append_format(unsigned char *, size_t, size_t *, const char *, ...);
static int parse_decimal_length(struct http_slice, size_t *);
static bool slices_equal_case(struct http_slice, struct http_slice);

int forward_request_to_backend(struct proxy_stream *stream,
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
        if (proxy_header_is_hop_by_hop(request->headers, field.name) ||
            http_field_name_equal(field.name, "host") ||
            http_field_name_equal(field.name, "content-length") ||
            http_field_name_equal(field.name, "expect") ||
            http_field_name_equal(field.name, "trailer")) continue;
        if (append_format(header, sizeof(header), &used, "%.*s: %.*s\r\n",
                          (int)field.name.length, field.name.data,
                          (int)field.value.length, field.value.data) < 0) return -1;
    }
    if (next < 0) { errno = EPROTO; return -1; }
    if (append_format(header, sizeof(header), &used, "Host: %s:%u\r\n",
                      address, (unsigned int)ntohs(backend->sin_port)) < 0) return -1;
    if (request->has_content_length || request->body.length != 0 ||
        http_slice_equal(request->method, "POST")) {
        if (append_format(header, sizeof(header), &used, "Content-Length: %zu\r\n",
                          request->body.length) < 0) return -1;
    }
    if (append_format(header, sizeof(header), &used,
                      "Connection: close\r\nVia: 1.1 jungle-proxy\r\n\r\n") < 0) return -1;
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

int prepare_backend_response_headers(struct proxy_stream *stream) {
    struct proxy_response_headers workspace = {0};
    struct proxy_response_headers *headers = &workspace;
    do {
        headers->raw_header_length = 0;
        while (headers->raw_header_length < sizeof(headers->raw_header)) {
            unsigned char byte;
            if (read_backend_byte(stream, &byte) < 0) return -1;
            headers->raw_header[headers->raw_header_length++] = byte;
            size_t length = headers->raw_header_length;
            if (length >= 4 && memcmp(headers->raw_header + length - 4, "\r\n\r\n", 4) == 0)
                break;
        }
        size_t length = headers->raw_header_length;
        if (length < 4 || memcmp(headers->raw_header + length - 4, "\r\n\r\n", 4) != 0) {
            errno = EPROTO; return -1;
        }
        if (parse_backend_headers(stream, headers) < 0) return -1;
        if (stream->status == 101) { errno = EPROTO; return -1; }
        /* Preserve informational fields, except 100 already handled at the proxy.
         * Retain them within the bounded output header until final headers validate. */
        if (stream->status > 100 && stream->status < 200 &&
            build_client_response_headers(stream, headers) < 0) return -1;
        if (deadline_reached(&stream->header_deadline)) { errno = ETIMEDOUT; return -1; }
    } while (stream->status < 200);
    if (build_client_response_headers(stream, headers) < 0 ||
        retain_trailer_connection_fields(stream, headers->fields) < 0) return -1;
    stream->client_header = malloc(headers->client_header_length);
    if (!stream->client_header) return -1;
    memcpy(stream->client_header, headers->client_header, headers->client_header_length);
    stream->client_header_length = headers->client_header_length;
    return 0;
}

int build_client_response_headers(struct proxy_stream *stream, struct proxy_response_headers *headers) {
    size_t *used = &headers->client_header_length;
    unsigned char *header = headers->client_header;
    size_t capacity = sizeof(headers->client_header);
    /* Client requests remain HTTP/1.1. Preserve status/reason, not backend version. */
    if (proxy_append_bytes(header, capacity, used, "HTTP/1.1 ", 9) < 0 ||
        proxy_append_bytes(header, capacity, used, headers->status_line.data + 9,
                     headers->status_line.length - 9) < 0 ||
        proxy_append_bytes(header, capacity, used, "\r\n", 2) < 0) return -1;
    size_t offset = 0;
    struct http_field field;
    int next;
    while ((next = http_next_field(headers->fields, &offset, &field)) > 0) {
        if (proxy_header_is_hop_by_hop(headers->fields, field.name) ||
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
    if (stream->status >= 200 &&
        append_format(header, capacity, used, "Connection: close\r\n") < 0) return -1;
    return append_format(header, capacity, used, "Via: 1.1 jungle-proxy\r\n\r\n");
}

int parse_backend_headers(struct proxy_stream *stream, struct proxy_response_headers *headers) {
    size_t status_length = 0;
    while (status_length + 1 < headers->raw_header_length &&
           memcmp(headers->raw_header + status_length, "\r\n", 2) != 0) ++status_length;
    const unsigned char *line = headers->raw_header;
    if (status_length < 13 ||
        (memcmp(line, "HTTP/1.1 ", 9) != 0 && memcmp(line, "HTTP/1.0 ", 9) != 0) ||
        line[12] != ' ' ||
        line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9') { errno = EPROTO; return -1; }
    for (size_t i = 13; i < status_length; ++i) {
        if ((line[i] < 32 && line[i] != '\t') || line[i] == 127) {
            errno = EPROTO; return -1;
        }
    }
    bool http10_response = memcmp(line, "HTTP/1.0 ", 9) == 0;
    stream->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    headers->status_line = (struct http_slice){line, status_length};
    headers->fields = (struct http_slice){
        line + status_length + 2, headers->raw_header_length - status_length - 4
    };
    bool length_seen = false, transfer_seen = false;
    stream->content_length = SIZE_MAX;
    size_t offset = 0;
    struct http_field field;
    int next;
    while ((next = http_next_field(headers->fields, &offset, &field)) > 0) {
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
    if (http10_response && (stream->status < 200 || transfer_seen)) goto invalid;
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

/* Preserve every final Connection row, including repeated/mixed-case rows.
 * Their combined raw size cannot exceed the already validated header limit. */
static int retain_trailer_connection_fields(struct proxy_stream *stream,
                                           struct http_slice fields) {
    if (stream->framing != BACKEND_CHUNKED) return 0;
    size_t offset = 0, total = 0;
    struct http_field field;
    while (offset < fields.length) {
        size_t start = offset;
        if (http_next_field(fields, &offset, &field) != 1) { errno = EPROTO; return -1; }
        if (http_field_name_equal(field.name, "connection")) total += offset - start;
    }
    if (total == 0) return 0;
    stream->connection_fields = malloc(total);
    if (!stream->connection_fields) return -1;
    offset = 0;
    size_t used = 0;
    while (offset < fields.length) {
        size_t start = offset;
        if (http_next_field(fields, &offset, &field) != 1) { errno = EPROTO; return -1; }
        if (http_field_name_equal(field.name, "connection")) {
            memcpy(stream->connection_fields + used, fields.data + start, offset - start);
            used += offset - start;
        }
    }
    stream->connection_fields_length = total;
    return 0;
}

bool proxy_header_is_hop_by_hop(struct http_slice fields, struct http_slice name) {
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
            while (position < field.value.length && proxy_is_token_byte(field.value.data[position]))
                ++position;
            if (slices_equal_case(name, (struct http_slice){
                    field.value.data + start, position - start})) return true;
            while (position < field.value.length && field.value.data[position] != ',') ++position;
            if (position < field.value.length) ++position;
        }
    }
    return false;
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

static int validate_connection_value(struct http_slice value) {
    size_t position = 0;
    for (;;) {
        while (position < value.length &&
               (value.data[position] == ' ' || value.data[position] == '\t')) ++position;
        size_t start = position;
        while (position < value.length && proxy_is_token_byte(value.data[position])) ++position;
        if (start == position) return -1;
        while (position < value.length &&
               (value.data[position] == ' ' || value.data[position] == '\t')) ++position;
        if (position == value.length) return 0;
        if (value.data[position++] != ',') return -1;
    }
}

int proxy_append_bytes(unsigned char *buffer, size_t capacity, size_t *used,
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

bool proxy_is_token_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}
