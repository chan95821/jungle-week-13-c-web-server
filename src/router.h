#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"

enum http_route {
    HTTP_ROUTE_HELLO, HTTP_ROUTE_ECHO, HTTP_ROUTE_STATIC,
    HTTP_ROUTE_TIME, HTTP_ROUTE_ADD
};

/* Borrows body storage owned by handle_client or a static literal. */
struct router_response {
    int status;
    const char *content_type;
    const void *body;
    size_t length;
    const char *allow;
};

/* Decide the route before receiving a body; return 0 or an HTTP error status. */
int http_route_request(const struct http_request *request,
                       enum http_route *route, const char **allow);

/* 기존 메모리 응답과 공통 오류 응답만 선택한다. 파일 성공 응답은 static_file,
 * 동적 성공 응답은 dynamic 모듈이 준비한다. 할당/해제/소켓 I/O는 하지 않는다. */
struct router_response router_prepare_response(const struct http_request *request,
                                               enum http_route route, int status,
                                               const char *allow);

#endif
