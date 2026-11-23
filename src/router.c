#include "router.h"

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
    if (http_slice_equal(r->path, "/greet")) {
        if (!get) { *allow = "GET"; return 405; }
        *route = HTTP_ROUTE_GREET;
        return 0;
    }
    if (r->path.length >= 8 && memcmp(r->path.data, "/static/", 8) == 0) {
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
        .kind = ROUTER_BODY_MEMORY,
        .file_fd = -1,
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