#define _POSIX_C_SOURCE 200809L
#include "proxy_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int read_trailers(struct proxy_stream *);

enum body_result read_backend_body_chunk(void *state, struct http_slice *chunk) {
    struct proxy_stream *stream = state;
    /* send_prepared_response finishes wire_header before the first next call.
     * Zero-body/header-send-failure paths release it in provider cleanup. */
    free(stream->client_header);
    stream->client_header = NULL;
    stream->client_header_length = 0;
    if (stream->body_complete || stream->framing == BACKEND_NO_BODY ||
        (stream->framing == BACKEND_LENGTH && stream->remaining == 0)) return BODY_END;
    if (!cancel_signal_is_running(stream->cancel)) { errno = ECANCELED; return BODY_ERROR; }
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

int parse_chunk_size(struct http_slice line, size_t *size) {
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
        while (position < line.length && proxy_is_token_byte(line.data[position])) ++position;
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
                while (position < line.length && proxy_is_token_byte(line.data[position])) ++position;
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
    unsigned char trailer_workspace[REQ_HEADER_MAX_BYTES];
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
            proxy_header_is_hop_by_hop((struct http_slice){
                stream->connection_fields, stream->connection_fields_length}, field.name) ||
            http_field_name_equal(field.name, "content-length") ||
            http_field_name_equal(field.name, "host") ||
            http_field_name_equal(field.name, "trailer") ||
            http_field_name_equal(field.name, "content-type") ||
            http_field_name_equal(field.name, "content-encoding") ||
            http_field_name_equal(field.name, "content-range")) {
            errno = EPROTO; return -1;
        }
        if (proxy_append_bytes(trailer_workspace, sizeof(trailer_workspace),
                          &total, line, length + 2) < 0) return -1;
    }
    if (total != 0) {
        stream->trailer_buffer = malloc(total);
        if (!stream->trailer_buffer) return -1;
        memcpy(stream->trailer_buffer, trailer_workspace, total);
    }
    stream->trailers = (struct http_slice){stream->trailer_buffer, total};
    return 0;
}
