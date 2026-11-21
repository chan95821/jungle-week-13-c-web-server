#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/http.h"

/* 파서는 소켓에 의존하지 않는다. 잘못된 입력 바이트만 넣어서 오류 종류를 검증한다. */
static void test_parser(void) {
    struct http_request request;
    const char *valid = "POST /echo?x=1 HTTP/1.1\r\nhOsT:\tlocalhost:8080 \r\n"
                        "content-length: 5\r\nConnection: keep-alive, CLOSE\r\n\r\n";
    assert(parse_http_request(valid, strlen(valid), &request) == 0);
    assert(request.method == HTTP_POST && request.http11 == 1);
    assert(strcmp(request.target, "/echo?x=1") == 0);
    assert(strcmp(request.host, "localhost:8080") == 0);
    assert(request.has_content_length && request.content_length == 5);
    assert(request.connection_close && request.body == NULL);

    const struct { const char *header; int status; } cases[] = {
        {"GET / HTTP/1.0\r\n\r\n", 0},
        {"GET / HTTP/1.1\r\nHost: x\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 0},
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: \r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x y\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded: y\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nBadHeader\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nBad: a\nb\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/1.1 extra\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/9.9\r\nHost: x\r\n\r\n", 505},
        {"GET / HTTP/x.y\r\nHost: x\r\n\r\n", 400},
        {"GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET /#fragment HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"DELETE / HTTP/1.1\r\nHost: x\r\n\r\n", 501},
        {"POST /echo HTTP/1.1\r\nHost: x\r\n\r\n", 411},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1, 1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999999999999999999\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", 413},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n", 417},
        {"GET / HTTP/1.1\r\nHost: x\r\nConnection: close,\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n\r\nextra", 400},
        {"GET / HTTP/1.1\nHost: x\n\n", 400},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int status = parse_http_request(cases[i].header, strlen(cases[i].header), &request);
        if (status != cases[i].status) fprintf(stderr, "Parser case %zu: %d\n", i, status);
        assert(status == cases[i].status);
    }
    const char with_nul[] = "GET / HTTP/1.1\r\nHost: x\r\nX: a\0b\r\n\r\n";
    assert(parse_http_request(with_nul, sizeof(with_nul) - 1, &request) == 400);
    assert(parse_http_request(NULL, 0, &request) == 400);
    assert(parse_http_request("x", REQ_HEADER_MAX_BYTES + 1, &request) == 431);
    char long_target[REQ_TARGET_MAX_BYTES + 128];
    memcpy(long_target, "GET /", 5);
    memset(long_target + 5, 'a', REQ_TARGET_MAX_BYTES);
    strcpy(long_target + 5 + REQ_TARGET_MAX_BYTES, " HTTP/1.1\r\nHost: x\r\n\r\n");
    assert(parse_http_request(long_target, strlen(long_target), &request) == 414);
}

static void test_routing(void) {
    struct http_request request = {0};
    struct http_response response;
    const struct { const char *target; int status; const char *body; } cases[] = {
        {"/", 200, "Hello"},
        {"/hello", 200, "Hello, World!\n"},
        {"/hello?name=Jungle", 200, "Hello, Jungle!\n"},
        {"/hello?x=1&name=A%20B+C", 200, "Hello, A B C!\n"},
        {"/hello?name=", 200, "Hello, !\n"},
        {"/hello?name=%00", 400, "Bad Request"},
        {"/hello?name=%0a", 400, "Bad Request"},
        {"/hello?name=%GG", 400, "Bad Request"},
        {"/hello?name=%", 400, "Bad Request"},
        {"/hello?name=a&name=b", 400, "Bad Request"},
        {"/hello?name", 400, "Bad Request"},
        {"/missing", 404, "Not Found"},
    };
    request.method = HTTP_GET;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        strcpy(request.target, cases[i].target);
        route_request(&request, &response);
        assert(response.status == cases[i].status);
        assert(response.body_length == strlen(cases[i].body));
        assert(memcmp(response.body, cases[i].body, response.body_length) == 0);
    }
    strcpy(request.target, "/echo");
    route_request(&request, &response);
    assert(response.status == 405 && strcmp(response.allow, "POST") == 0);
    request.method = HTTP_POST;
    unsigned char binary[] = {0, 255, '\r', '\n', 'x'};
    request.body = binary;
    request.body_length = sizeof(binary);
    route_request(&request, &response);
    assert(response.status == 200 && response.body_length == sizeof(binary));
    assert(response.body == request.body); /* 복사 없이 빌린 버퍼임을 확인. */
    assert(memcmp(response.body, binary, sizeof(binary)) == 0);
    strcpy(request.target, "/hello");
    route_request(&request, &response);
    assert(response.status == 405 && strcmp(response.allow, "GET") == 0);

    request.method = HTTP_GET;
    strcpy(request.target, "/hello?name=");
    size_t prefix = strlen(request.target);
    memset(request.target + prefix, 'a', HELLO_NAME_MAX_BYTES);
    request.target[prefix + HELLO_NAME_MAX_BYTES] = '\0';
    route_request(&request, &response);
    assert(response.status == 200 && response.body_length == HELLO_NAME_MAX_BYTES + 9);
    request.target[prefix + HELLO_NAME_MAX_BYTES] = 'a';
    request.target[prefix + HELLO_NAME_MAX_BYTES + 1] = '\0';
    route_request(&request, &response);
    assert(response.status == 400);
}

int main(void) {
    test_parser();
    test_routing();
    puts("[PASS] test_http (parser + routing)");
    return 0;
}
