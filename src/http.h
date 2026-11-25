#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>

/* Provisional project bounds, not RFC recommendations or measured optima. */
#define REQ_HEADER_MAX_BYTES 8192
#define REQ_BODY_MAX_BYTES (64 * 1024)

struct http_slice {
    const unsigned char *data;
    size_t length;
};

/* Slices borrow caller-owned request storage (B: event_connection).
 * This struct frees nothing; the owner keeps storage alive through response/cleanup. */
struct http_request {
    struct http_slice method;
    struct http_slice target;
    struct http_slice version;
    struct http_slice path;
    struct http_slice query;
    struct http_slice host;
    struct http_slice headers; /* Raw validated fields, including their CRLF. */
    struct http_slice body;
    size_t content_length;
    bool has_content_length;
    bool expect_continue;
    bool connection_close;
    bool connection_keep_alive;
};

/* Return 0 on success, otherwise an HTTP error status. Never modify input. */
int http_parse_request(const unsigned char *data, size_t length,
                       struct http_request *request);
/* Compare slice bytes exactly against a NUL-terminated ASCII string. */
bool http_slice_equal(struct http_slice value, const char *text);
/* Return standard reason phrase for a supported HTTP status code. */
const char *http_reason(int status);

struct http_field {
    struct http_slice name;
    struct http_slice value;
};
/* 1: validated field, 0: end, -1: invalid syntax. Does not allocate or mutate. */
int http_next_field(struct http_slice fields, size_t *offset, struct http_field *field);
bool http_field_name_equal(struct http_slice name, const char *lowercase_name);

#endif
