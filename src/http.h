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

/* All slices borrow storage owned by handle_client; this struct frees nothing. */
struct http_request {
    struct http_slice method;
    struct http_slice target;
    struct http_slice version;
    struct http_slice path;
    struct http_slice query;
    struct http_slice host;
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
bool http_slice_equal(struct http_slice value, const char *text);
const char *http_reason(int status);

#endif
