#ifndef PROXY_INTERNAL_H
#define PROXY_INTERNAL_H

/* Private to the sequential proxy implementation and its unit tests. */
#include "proxy.h"
#include "socket_io.h"

enum backend_framing { BACKEND_LENGTH, BACKEND_CHUNKED, BACKEND_CLOSE, BACKEND_NO_BODY };
enum { PROXY_OUTPUT_HEADER_BYTES = 2 * REQ_HEADER_MAX_BYTES + 256 };

/* Owns backend_fd and optional heap blocks; never borrows header workspace slices. */
struct proxy_stream {
    int backend_fd;
    const volatile sig_atomic_t *running;
    struct timespec header_deadline, body_deadline;
    bool body_started;
    unsigned char buffer[PROXY_STREAM_BYTES];
    size_t buffer_offset, buffer_length;
    unsigned char *client_header;
    size_t client_header_length;
    unsigned char *connection_fields;
    size_t connection_fields_length;
    unsigned char *trailer_buffer;
    struct http_slice trailers;
    enum backend_framing framing;
    size_t content_length;
    bool has_content_length;
    size_t remaining, chunk_remaining;
    bool chunk_needs_crlf, body_complete;
    int status;
};

/* Preparation-only stack workspace. fields/status_line borrow raw_header. */
struct proxy_response_headers {
    unsigned char raw_header[REQ_HEADER_MAX_BYTES];
    size_t raw_header_length;
    unsigned char client_header[PROXY_OUTPUT_HEADER_BYTES];
    size_t client_header_length;
    struct http_slice fields, status_line;
};

int connect_backend(struct proxy_stream *, const struct sockaddr_in *);
int forward_request_to_backend(struct proxy_stream *, const struct sockaddr_in *,
                               const struct http_request *);
/* 순수 헤더 생성/검증을 blocking I/O에서 분리하여 이벤트 경로에서도 재사용한다. */
int build_backend_request_header(const struct sockaddr_in *, const struct http_request *,
                                  unsigned char *, size_t, size_t *);
/* 완전한 CRLFCRLF 블록 하나: 0=interim, 1=final, -1=잘못된 응답. */
int consume_backend_header_block(struct proxy_stream *, struct proxy_response_headers *);
bool backend_trailer_is_allowed(const struct proxy_stream *, struct http_field);
/* Copies only the completed wire header and final chunked Connection fields. */
int prepare_backend_response_headers(struct proxy_stream *);
int parse_backend_headers(struct proxy_stream *, struct proxy_response_headers *);
int build_client_response_headers(struct proxy_stream *, struct proxy_response_headers *);
enum body_result read_backend_body_chunk(void *, struct http_slice *);
void cleanup_backend_response(void *);
int make_deadline(struct timespec *, int);
bool deadline_reached(const struct timespec *);
int fill_backend_buffer(struct proxy_stream *);
int read_backend_byte(struct proxy_stream *, unsigned char *);
int read_backend_line(struct proxy_stream *, unsigned char *, size_t, size_t *);
bool proxy_header_is_hop_by_hop(struct http_slice, struct http_slice);
int proxy_append_bytes(unsigned char *, size_t, size_t *, const void *, size_t);
bool proxy_is_token_byte(unsigned char);
int parse_chunk_size(struct http_slice, size_t *);

#endif
