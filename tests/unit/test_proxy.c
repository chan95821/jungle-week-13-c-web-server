#define _POSIX_C_SOURCE 200809L
/* 내부 파서의 경계 검증. 테스트만 위해 production API를 늘리지 않는다. */
#include "../../src/proxy.c"
#include <assert.h>

static void test_endpoints_and_routes(void) {
    struct sockaddr_in address;
    assert(proxy_parse_endpoint("127.0.0.1:8001", &address) == 0);
    assert(ntohs(address.sin_port) == 8001);
    const char *bad[] = {"localhost:80", "127.0.0.1:0", "127.0.0.1:65536",
                         "127.0.0.1:", "127.0.0.1:8x", "999.0.0.1:80", "::1:80"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(proxy_parse_endpoint(bad[i], &address) < 0);
    struct http_request request = {0};
    request.path = (struct http_slice){(const unsigned char *)"/api/submissions", 16};
    request.method = (struct http_slice){(const unsigned char *)"POST", 4};
    const char *allow = NULL;
    assert(proxy_select_route(&request, &allow) == PROXY_SUBMISSION);
    request.method = (struct http_slice){(const unsigned char *)"GET", 3};
    assert(proxy_select_route(&request, &allow) == -405);
    assert(strcmp(allow, "POST") == 0);
    request.method = (struct http_slice){(const unsigned char *)"DELETE", 6};
    assert(proxy_select_route(&request, &allow) == -501);
}

static void test_header_contracts(void) {
    static const unsigned char raw[] =
        "POST /api/submissions?raw=%2f HTTP/1.1\r\nHost: original.example\r\n"
        "X-Hop: remove\r\nConnection: X-Hop, keep-alive\r\n"
        "Content-Type: application/octet-stream\r\nContent-Length: 3\r\n"
        "Expect: 100-continue\r\n\r\n";
    struct http_request request;
    assert(http_parse_request(raw, sizeof(raw) - 1, &request) == 0);
    request.body = (struct http_slice){(const unsigned char *)"abc", 3};
    unsigned char wire[PROXY_WIRE_BYTES + 1];
    size_t length;
    assert(build_request(raw, sizeof(raw) - 1, &request, wire, &length));
    wire[length] = 0;
    assert(strstr((char *)wire, "POST /api/submissions?raw=%2f HTTP/1.1\r\n"));
    assert(strstr((char *)wire, "Host: original.example\r\n"));
    assert(!strstr((char *)wire, "X-Hop:"));
    assert(!strstr((char *)wire, "Expect:"));
    assert(strstr((char *)wire, "Content-Length: 3\r\n"));
    assert(strstr((char *)wire, "Connection: close\r\n"));

    static const unsigned char response[] =
        "HTTP/1.1 201 Created\r\nContent-Length: 3\r\n"
        "X-Hop: remove\r\nConnection: X-Hop\r\n"
        "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\n";
    struct response_meta meta;
    assert(parse_response(response, sizeof(response) - 1, &meta));
    assert(meta.status == 201 && meta.has_length && meta.length == 3);
    assert(build_response(response, sizeof(response) - 1, &meta, wire, &length));
    wire[length] = 0;
    assert(!strstr((char *)wire, "X-Hop:"));
    assert(strstr((char *)wire, "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"));

    const char *bad[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 184467440737095516160\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\n Bad: folded\r\n\r\n",
        "HTTP/1.1 204 No Content\r\nContent-Length: 3\r\n\r\n",
        "HTTP/1.1 101 Switching Protocols\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection: close,\r\n\r\n",
        "HTTP/1.1 20x Bad\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!parse_response((const unsigned char *)bad[i], strlen(bad[i]), &meta));
}

static void test_chunk_sizes(void) {
    size_t size;
    const char *good[] = {"0", "aF", "3;foo=bar", "3;foo=\"a\\\"b\"", "3 ; foo = bar"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i)
        assert(chunk_size((const unsigned char *)good[i], strlen(good[i]), &size));
    const char *bad[] = {"", "-1", "+1", "xyz", "1;", "1;foo=", "1;foo=\"unterminated",
                         "ffffffffffffffffffffffffffffffff", "1;foo=\"a\001b\""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!chunk_size((const unsigned char *)bad[i], strlen(bad[i]), &size));
}

int main(void) {
    test_endpoints_and_routes();
    test_header_contracts();
    test_chunk_sizes();
    puts("proxy parser contracts passed");
    return 0;
}
