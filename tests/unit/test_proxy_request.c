#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/proxy.h"

/* Check that the next connection receives only the selected and generated fields. */
static void rewrites_request_headers(void) {
    const char wire[] =
        "POST /api/submissions?q=%20 HTTP/1.1\r\n"
        "X-Demo: local-only\r\nHost: demo.example\r\nConnection: close, x-demo\r\n"
        "X-Other: local-only\r\nConnection: X-OTHER\r\nKeep-Alive: timeout=5\r\n"
        "Expect: 100-continue\r\nContent-Type: application/octet-stream\r\n"
        "Via: 1.1 previous\r\nAccept: a\r\nAccept: b\r\nContent-Length: 003\r\n\r\n";
    const char expected[] =
        "POST /api/submissions?q=%20 HTTP/1.1\r\n"
        "Host: demo.example\r\nContent-Type: application/octet-stream\r\n"
        "Via: 1.1 previous\r\nAccept: a\r\nAccept: b\r\n"
        "Via: 1.1 jungle-proxy\r\nConnection: close\r\nContent-Length: 3\r\n\r\n";
    struct http_request request;
    assert(http_parse_request((const unsigned char *)wire, sizeof(wire) - 1, &request) == 0);

    char output[sizeof(expected) - 1];
    size_t length;
    assert(build_proxy_request_headers(&request, output, sizeof(output), &length) == 0);
    assert(length == sizeof(output) && memcmp(output, expected, length) == 0);

    char short_output[sizeof(output) - 1];
    assert(build_proxy_request_headers(&request, short_output, sizeof(short_output), &length) != 0);
    assert(length == 0);
}

/* Check that nominating a protected field does not yield a sendable request. */
static void rejects_protected_connection_fields(void) {
    const char *fields[] = {"HoSt", "Content-Length", "Transfer-Encoding"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        char wire[160];
        int count = snprintf(wire, sizeof(wire),
                             "GET / HTTP/1.1\r\nHost: demo.example\r\n"
                             "Connection: close\r\nConnection: %s\r\n\r\n", fields[i]);
        assert(count > 0 && (size_t)count < sizeof(wire));
        struct http_request request;
        assert(http_parse_request((const unsigned char *)wire, (size_t)count, &request) == 0);

        char output[256];
        size_t length = 1;
        assert(build_proxy_request_headers(&request, output, sizeof(output), &length) == 400);
        assert(length == 0);
    }
}

/* Verify request header rewriting and rejection before any network I/O. */
int main(void) {
    rewrites_request_headers();
    rejects_protected_connection_fields();
    puts("[PASS] test_proxy_request");
    return 0;
}
