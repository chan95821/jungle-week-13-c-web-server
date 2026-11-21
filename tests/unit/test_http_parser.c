#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/router.h"

static void valid_request(void) {
    unsigned char wire[] = "POST /echo?x=%20 HTTP/1.1\r\nhOsT:\tlocalhost:8080 \t\r\n"
                           "Content-Length: 5\r\nConnection: Keep-Alive, close\r\n"
                           "Expect: 100-continue\r\nX-Unused: value\r\n\r\n";
    unsigned char original[sizeof(wire)];
    memcpy(original, wire, sizeof(wire));
    struct http_request request;
    assert(http_parse_request(wire, sizeof(wire) - 1, &request) == 0);
    assert(memcmp(wire, original, sizeof(wire)) == 0);
    assert(http_slice_equal(request.method, "POST"));
    assert(http_slice_equal(request.version, "HTTP/1.1"));
    assert(http_slice_equal(request.path, "/echo"));
    assert(http_slice_equal(request.host, "localhost:8080"));
    assert(request.method.data == wire);
    assert(request.target.data >= wire && request.target.data < wire + sizeof(wire));
    assert(request.content_length == 5 && request.has_content_length);
    assert(request.expect_continue && request.connection_close && request.connection_keep_alive);
    assert(request.body.length == 0 && request.body.data == NULL);
    enum http_route route;
    const char *allow;
    assert(http_route_request(&request, &route, &allow) == 0 && route == HTTP_ROUTE_ECHO);
}

static void basic_statuses(void) {
    static const struct { const char *wire; int status; } cases[] = {
        {"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: localhost\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: [::1]:8080\r\nContent-Length: 0\r\n\r\n", 0},
        {"GET /%68ello HTTP/1.1\r\nHost: localhost\r\n\r\n", 0},
        {"GET /hello HTTP/1.0\r\nHost: localhost\r\n\r\n", 505},
        {"GET http://localhost/hello HTTP/1.1\r\nHost: localhost\r\n\r\n", 501},
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a\r\nHost: a\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a b\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: []\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a\r\n Folded: b\r\n\r\n", 400},
        {"GET /bad%2 HTTP/1.1\r\nHost: a\r\n\r\n", 400},
        {"GET /bad#fragment HTTP/1.1\r\nHost: a\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: +1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 18446744073709551616\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65535\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65536\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65537\r\n\r\n", 413},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked; x=1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, chunked\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nExpect: other\r\n\r\n", 417},
        {"GET / HTTP/1.1\r\nHost: a\r\nConnection: close,\r\n\r\n", 400}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct http_request request;
        int status = http_parse_request((const unsigned char *)cases[i].wire,
                                         strlen(cases[i].wire), &request);
        if (status != cases[i].status) fprintf(stderr, "parser case %zu: %d != %d\n", i, status, cases[i].status);
        assert(status == cases[i].status);
    }
    static const unsigned char nul[] = "GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n";
    struct http_request request;
    assert(http_parse_request(nul, sizeof(nul) - 1, &request) == 400);
}

static void routes(void) {
    static const struct { const char *method; const char *target; int status; const char *allow; } cases[] = {
        {"GET", "/", 0, NULL}, {"GET", "/hello?x=1", 0, NULL},
        {"POST", "/echo", 0, NULL}, {"GET", "/echo", 405, "POST"},
        {"POST", "/hello", 405, "GET"}, {"GET", "/missing", 404, NULL},
        {"GET", "/%68ello", 404, NULL}, {"GET", "/a/../hello", 404, NULL},
        {"DELETE", "/hello", 501, NULL}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char wire[256];
        int length = snprintf(wire, sizeof(wire), "%s %s HTTP/1.1\r\nHost: a\r\n\r\n", cases[i].method, cases[i].target);
        assert(length > 0 && (size_t)length < sizeof(wire));
        struct http_request request;
        assert(http_parse_request((const unsigned char *)wire, (size_t)length, &request) == 0);
        enum http_route route;
        const char *allow;
        assert(http_route_request(&request, &route, &allow) == cases[i].status);
        if (cases[i].allow) assert(allow && strcmp(allow, cases[i].allow) == 0);
        else assert(allow == NULL);
    }
}

static void response_selection(void) {
    unsigned char bytes[] = {'a', 0, 'b', '\r', '\n'};
    struct http_request request = {0};
    request.body = (struct http_slice){bytes, sizeof(bytes)};
    struct router_response response =
        router_prepare_response(&request, HTTP_ROUTE_ECHO, 200, NULL);
    assert(response.status == 200 && response.allow == NULL);
    assert(strcmp(response.content_type, "application/octet-stream") == 0);
    assert(response.body == bytes && response.length == sizeof(bytes));

    request.body = (struct http_slice){NULL, 0};
    response = router_prepare_response(&request, HTTP_ROUTE_ECHO, 200, NULL);
    assert(response.body == NULL && response.length == 0);
    response = router_prepare_response(&request, HTTP_ROUTE_HELLO, 200, NULL);
    assert(strcmp(response.content_type, "text/plain") == 0);
    assert(response.length == 5 && memcmp(response.body, "Hello", 5) == 0);
    response = router_prepare_response(&request, HTTP_ROUTE_HELLO, 405, "GET");
    assert(response.status == 405 && strcmp(response.allow, "GET") == 0);
    assert(response.length == strlen("Method Not Allowed"));
    assert(memcmp(response.body, "Method Not Allowed", response.length) == 0);
}
int main(void) {
    valid_request();
    basic_statuses();
    routes();
    response_selection();
    puts("[PASS] test_http_parser");
    return 0;
}
