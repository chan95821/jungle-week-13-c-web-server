#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../src/server.h"

static void test_dispatch_routes(void) {
    HttpRequest req;
    HttpResponse resp;

    /* GET /hello */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "GET");
    strcpy(req.uri, "/hello");
    dispatch_request(&req, &resp);
    assert(resp.status_code == 200);
    assert(resp.body_len == 5 && memcmp(resp.body, "Hello", 5) == 0);
    free_http_response(&resp);

    /* GET /hello?a=1 (query string 포함) */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "GET");
    strcpy(req.uri, "/hello?a=1");
    dispatch_request(&req, &resp);
    assert(resp.status_code == 200);
    assert(resp.body_len == 5 && memcmp(resp.body, "Hello", 5) == 0);
    free_http_response(&resp);

    /* GET / (Iter 1 회귀 호환) */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "GET");
    strcpy(req.uri, "/");
    dispatch_request(&req, &resp);
    assert(resp.status_code == 200);
    assert(resp.body_len == 5 && memcmp(resp.body, "Hello", 5) == 0);
    free_http_response(&resp);

    /* POST /echo 바이너리 본문 + Content-Type 유지 */
    uint8_t bin_body[7] = {'a', 'b', 'c', '\0', 'd', 'e', 'f'};
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "POST");
    strcpy(req.uri, "/echo");
    strcpy(req.content_type, "application/json");
    req.has_content_type = 1;
    req.has_content_length = 1;
    req.content_length = sizeof(bin_body);
    req.body = bin_body;
    req.body_len = sizeof(bin_body);
    dispatch_request(&req, &resp);
    assert(resp.status_code == 200);
    assert(strcmp(resp.content_type, "application/json") == 0);
    assert(resp.body_len == 7 && memcmp(resp.body, bin_body, 7) == 0);
    free_http_response(&resp);

    /* POST /echo Content-Length: 0 */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "POST");
    strcpy(req.uri, "/echo");
    req.has_content_length = 1;
    req.content_length = 0;
    dispatch_request(&req, &resp);
    assert(resp.status_code == 200);
    assert(strcmp(resp.content_type, "application/octet-stream") == 0);
    assert(resp.body_len == 0);
    free_http_response(&resp);

    /* POST /echo Content-Length 누락 -> 411 */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "POST");
    strcpy(req.uri, "/echo");
    req.has_content_length = 0;
    dispatch_request(&req, &resp);
    assert(resp.status_code == 411);
    free_http_response(&resp);

    /* GET /unknown -> 404 */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "GET");
    strcpy(req.uri, "/unknown");
    dispatch_request(&req, &resp);
    assert(resp.status_code == 404);
    assert(resp.body_len > 0);
    free_http_response(&resp);

    /* GET /echo, POST /hello -> 405 */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "GET");
    strcpy(req.uri, "/echo");
    dispatch_request(&req, &resp);
    assert(resp.status_code == 405);
    free_http_response(&resp);

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "POST");
    strcpy(req.uri, "/hello");
    req.has_content_length = 1;
    dispatch_request(&req, &resp);
    assert(resp.status_code == 405);
    free_http_response(&resp);

    /* POST /unknown, POST /hello (Content-Length 누락) -> 411보다 404/405 우선 */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "POST");
    strcpy(req.uri, "/unknown");
    req.has_content_length = 0;
    dispatch_request(&req, &resp);
    assert(resp.status_code == 404);
    free_http_response(&resp);

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    strcpy(req.method, "POST");
    strcpy(req.uri, "/hello");
    req.has_content_length = 0;
    dispatch_request(&req, &resp);
    assert(resp.status_code == 405);
    free_http_response(&resp);
}

static void test_send_http_response_binary(void) {
    int sv[2];
    HttpResponse resp;
    const uint8_t payload[5] = {'x', '\0', 'y', '\0', 'z'};
    char recv_buf[512];

    memset(&resp, 0, sizeof(resp));
    resp.status_code = 200;
    resp.reason = "OK";
    resp.content_type = "application/octet-stream";
    resp.body = payload;
    resp.body_len = sizeof(payload);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send_http_response(sv[0], &resp) == 0);
    close(sv[0]);

    ssize_t n = recv(sv[1], recv_buf, sizeof(recv_buf), 0);
    assert(n > 0);
    const char *expected_head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n";
    size_t hlen = strlen(expected_head);
    assert((size_t)n == hlen + sizeof(payload));
    assert(memcmp(recv_buf, expected_head, hlen) == 0);
    assert(memcmp(recv_buf + hlen, payload, sizeof(payload)) == 0);
    close(sv[1]);
}

int main(void) {
    test_dispatch_routes();
    test_send_http_response_binary();
    printf("[PASS] test_dispatch\n");
    return 0;
}
