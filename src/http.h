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
    /* 시작 줄·빈 줄을 제외한, http_parse_request() 검증 완료 헤더 구간 슬라이스. */
    struct http_slice headers;
    struct http_slice body;
    size_t content_length;
    bool has_content_length;
    bool expect_continue;
    bool connection_close;
    bool connection_keep_alive;
};

/* 헤더 해석 결과만 담는다. slices는 입력을 빌리며, 클라이언트 송신용 router_response와 다르다. */
struct http_response_headers {
    int status;
    struct http_slice content_type;
    struct http_slice headers;
    size_t content_length;
    bool has_content_length;
    bool has_transfer_encoding;
};

/* 완성된 응답 헤더 블록을 해석한다. 성공 0, 잘못된 백엔드 응답 502; 입력 변경·할당 없음. */
int http_parse_response_headers(const unsigned char *data, size_t length,
                                 struct http_response_headers *response);

/* Return 0 on success, otherwise an HTTP error status. Never modify input. */
int http_parse_request(const unsigned char *data, size_t length,
                       struct http_request *request);
/* request.headers 복사본을 순회해 다음 헤더의 name/value를 원문에서 빌려 온다(값 OWS 제거, 끝이면 false). */
bool http_header_next(struct http_slice *cursor, struct http_slice *name,
                      struct http_slice *value);
/* Compare slice bytes exactly against a NUL-terminated ASCII string. */
bool http_slice_equal(struct http_slice value, const char *text);
/* Return standard reason phrase for a supported HTTP status code. */
const char *http_reason(int status);

#endif
