#include "proxy.h"

#include <string.h>
#include <strings.h>

/* 검증된 headers의 모든 Connection 줄을 순회해 쉼표로 구분된 토큰 중 field가 있는지 대소문자 무시로 검사한다.
 * 값 복사본 커서만 전진시키므로 입력 버퍼를 바꾸지 않으며 I/O도 없다. */
bool connection_lists_header(struct http_slice headers, struct http_slice field) {
    struct http_slice name, value;
    while (http_header_next(&headers, &name, &value)) {
        if (name.length != sizeof("Connection") - 1 ||
            strncasecmp((const char *)name.data, "Connection", name.length) != 0) continue;

        size_t i = 0;
        while (i < value.length) {
            while (i < value.length && (value.data[i] == ' ' || value.data[i] == '\t')) ++i;
            size_t start = i;
            while (i < value.length && value.data[i] != ',') ++i;
            size_t end = i;
            while (end > start && (value.data[end - 1] == ' ' || value.data[end - 1] == '\t')) --end;
            if (field.length == end - start &&
                strncasecmp((const char *)field.data,
                            (const char *)value.data + start, field.length) == 0) return true;
            if (i < value.length) ++i;
        }
    }
    return false;
}

/* Connection 옵션에 보호 필드(Host, Content-Length, Transfer-Encoding)가 포함되면 400, 없으면 0을 반환한다.
 * http_select_handler()(바디 수신 전)와 build_proxy_request_headers()에서 호출하며 I/O는 없다. */
int validate_proxy_headers(const struct http_request *request) {
    static const char *const protected[] = {"Host", "Content-Length", "Transfer-Encoding"};
    for (size_t i = 0; i < sizeof(protected) / sizeof(protected[0]); ++i) {
        struct http_slice field = {(const unsigned char *)protected[i], strlen(protected[i])};
        if (connection_lists_header(request->headers, field)) return 400;
    }
    return 0;
}

/* 헤더 의미를 이번 고정 길이 중계 범위에 대조한다. 백엔드의 정상 4xx/5xx는 바꾸지 않는다.
 * 최종 응답 수신 루프가 아직 없으므로 1xx는 중계하지 않고 502로 거부하며,
 * 101 업그레이드와 미지원 전송 형식도 거부한다. */
int validate_backend_response_headers(const struct http_response_headers *response) {
    static const unsigned char length_name[] = "Content-Length";
    static const unsigned char transfer_name[] = "Transfer-Encoding";
    if (connection_lists_header(response->headers,
                                 (struct http_slice){length_name, sizeof(length_name) - 1}) ||
        connection_lists_header(response->headers,
                                 (struct http_slice){transfer_name, sizeof(transfer_name) - 1})) return 502;
    if (response->has_transfer_encoding || response->status < 200 ||
        response->status == 101) return 502;

    if (response->status == 204) return response->has_content_length ? 502 : 0;
    if (response->status == 304) return 0;
    if (response->status == 205 && response->has_content_length && response->content_length != 0) return 502;
    return response->has_content_length ? 0 : 502;
}
