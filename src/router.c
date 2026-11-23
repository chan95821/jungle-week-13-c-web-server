#include "router.h"
#include "static_file.h"

#include <string.h>

int http_route_request(const struct http_request *r,
                       enum http_route *route, const char **allow) {
    *allow = NULL;
    bool get = http_slice_equal(r->method, "GET");
    bool post = http_slice_equal(r->method, "POST");
    if (!get && !post) return 501;
    if (http_slice_equal(r->path, "/") || http_slice_equal(r->path, "/hello")) {
        if (!get) { *allow = "GET"; return 405; }
        *route = HTTP_ROUTE_HELLO;
        return 0;
    }
    if (http_slice_equal(r->path, "/echo")) {
        if (!post) { *allow = "POST"; return 405; }
        *route = HTTP_ROUTE_ECHO;
        return 0;
    }
    /* 동적 기능도 라우터는 경로/메서드만 판정한다. 본문 해석과 계산은 dynamic.c,
     * 파일 읽기는 static_file.c에 따로 두고 실제 송신은 server.c가 맡는다. */
    if (http_slice_equal(r->path, "/api/time")) {
        if (!get) { *allow = "GET"; return 405; }
        *route = HTTP_ROUTE_TIME;
        return 0;
    }
    if (http_slice_equal(r->path, "/api/add")) {
        if (!post) { *allow = "POST"; return 405; }
        *route = HTTP_ROUTE_ADD;
        return 0;
    }
    /* 라우터는 어떤 처리인지 판정만 한다. 파일 존재/권한 확인은 파일 I/O 계층에
     * 맡겨서 순수 파싱·라우팅 테스트가 파일시스템에 의존하지 않게 한다. */
    const size_t prefix = sizeof(STATIC_URL_PREFIX) - 1;
    if (r->path.length >= prefix && memcmp(r->path.data, STATIC_URL_PREFIX, prefix) == 0) {
        if (!get) { *allow = "GET"; return 405; }
        *route = HTTP_ROUTE_STATIC;
        return 0;
    }
    return 404;
}

struct router_response router_prepare_response(const struct http_request *request,
                                               enum http_route route, int status,
                                               const char *allow) {
    const char *reason = http_reason(status);
    struct router_response response = {
        .status = status,
        .content_type = "text/plain",
        .body = reason,
        .length = strlen(reason),
        .allow = allow
    };
    if (status == 200 && route == HTTP_ROUTE_ECHO) {
        response.content_type = "application/octet-stream";
        response.body = request->body.data;
        response.length = request->body.length;
    } else if (status == 200) {
        response.body = "Hello";
        response.length = 5;
    }
    return response;
}
