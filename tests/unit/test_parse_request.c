#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/server.h"

static void test_request_line_valid(void) {
    HttpRequest req;
    HttpStatus err = HTTP_STATUS_NONE;

    memset(&req, 0, sizeof(req));
    assert(parse_request_line("GET /hello HTTP/1.1", 19, &req, &err) == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.uri, "/hello") == 0);
    assert(strcmp(req.version, "HTTP/1.1") == 0);

    memset(&req, 0, sizeof(req));
    assert(parse_request_line("POST /echo HTTP/1.0", 19, &req, &err) == 0);
    assert(strcmp(req.method, "POST") == 0);
    assert(strcmp(req.uri, "/echo") == 0);
    assert(strcmp(req.version, "HTTP/1.0") == 0);

    /* URI 정확히 2048B 경계 */
    char line_max[2100];
    memcpy(line_max, "GET /", 5);
    memset(line_max + 5, 'a', REQ_URI_MAX_BYTES - 1);
    memcpy(line_max + 4 + REQ_URI_MAX_BYTES, " HTTP/1.1", 9);
    size_t total = 4 + REQ_URI_MAX_BYTES + 9;
    memset(&req, 0, sizeof(req));
    assert(parse_request_line(line_max, total, &req, &err) == 0);
    assert(strlen(req.uri) == REQ_URI_MAX_BYTES);
}

static void test_request_line_invalid(void) {
    HttpRequest req;
    HttpStatus err = HTTP_STATUS_NONE;

    const char *bad_400[] = {
        "",
        "GET /hello",
        "GET /hello HTTP/1.1 extra",
        "GET  /hello HTTP/1.1",
        "GET\t/hello HTTP/1.1",
        "GET hello HTTP/1.1",
        "BAD@METH / HTTP/1.1",
        "GET /hello FOO/1.1",
        "GET /hello HTTP/1.10",
        "GET /he\x7fllo HTTP/1.1",
        "GET /\xff HTTP/1.1",
    };
    for (size_t i = 0; i < sizeof(bad_400) / sizeof(bad_400[0]); i++) {
        memset(&req, 0, sizeof(req));
        err = HTTP_STATUS_NONE;
        assert(parse_request_line(bad_400[i], strlen(bad_400[i]), &req, &err) == -1);
        assert(err == 400);
    }

    /* 미지원 메서드 -> 405 */
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_line("PUT /echo HTTP/1.1", 18, &req, &err) == -1);
    assert(err == 405);

    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_line("DELETE /hello HTTP/1.1", 22, &req, &err) == -1);
    assert(err == 405);

    /* URI 2049B -> 414 */
    char line_over[2100];
    memcpy(line_over, "GET /", 5);
    memset(line_over + 5, 'a', REQ_URI_MAX_BYTES);
    memcpy(line_over + 5 + REQ_URI_MAX_BYTES, " HTTP/1.1", 9);
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_line(line_over, 5 + REQ_URI_MAX_BYTES + 9, &req, &err) == -1);
    assert(err == 414);

    /* 미지원 버전 -> 505 */
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_line("GET /hello HTTP/2.0", 19, &req, &err) == -1);
    assert(err == 505);
}

static void test_header_line_syntax(void) {
    const char *name = NULL;
    const char *val = NULL;
    size_t nlen = 0;
    size_t vlen = 0;

    const char *h1 = "Host: localhost:8080";
    assert(parse_header_line(h1, strlen(h1), &name, &nlen, &val, &vlen) == 0);
    assert(nlen == 4 && memcmp(name, "Host", 4) == 0);
    assert(vlen == 14 && memcmp(val, "localhost:8080", 14) == 0);

    const char *h2 = "Content-Length:   42 \t";
    assert(parse_header_line(h2, strlen(h2), &name, &nlen, &val, &vlen) == 0);
    assert(nlen == 14 && memcmp(name, "Content-Length", 14) == 0);
    assert(vlen == 2 && memcmp(val, "42", 2) == 0);

    const char *h3 = "X-Empty:";
    assert(parse_header_line(h3, strlen(h3), &name, &nlen, &val, &vlen) == 0);
    assert(nlen == 7 && vlen == 0);

    const char *bad_lines[] = {
        "NoColonHeader",
        ": value",
        "Bad Name: value",
        "Content-Length : 5",
        "Bad@Name: value",
        "X-Test: a\x07" "b",
        "X-Test: \xff",
    };
    for (size_t i = 0; i < sizeof(bad_lines) / sizeof(bad_lines[0]); i++) {
        assert(parse_header_line(bad_lines[i], strlen(bad_lines[i]),
                                 &name, &nlen, &val, &vlen) == -1);
    }
}

static void test_request_head_valid_and_errors(void) {
    HttpRequest req;
    HttpStatus err = HTTP_STATUS_NONE;

    /* 정상 HTTP/1.1 + 대소문자 무시 Content-Length + 모르는 헤더 */
    const char *ok_11 =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "content-length: 1048576\r\n"
        "Content-Type: text/plain\r\n"
        "X-Custom: ok\r\n"
        "\r\n";
    memset(&req, 0, sizeof(req));
    assert(parse_request_head(ok_11, strlen(ok_11), &req, &err) == 0);
    assert(req.has_host == 1 && strcmp(req.host, "localhost") == 0);
    assert(req.has_content_length == 1 && req.content_length == 1048576);
    assert(req.has_content_type == 1 && strcmp(req.content_type, "text/plain") == 0);

    /* HTTP/1.0 Host 누락 허용 */
    const char *ok_10 = "GET /hello HTTP/1.0\r\n\r\n";
    memset(&req, 0, sizeof(req));
    assert(parse_request_head(ok_10, strlen(ok_10), &req, &err) == 0);

    /* HTTP/1.1 Host 누락 -> 400 */
    const char *no_host_11 = "GET /hello HTTP/1.1\r\n\r\n";
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_head(no_host_11, strlen(no_host_11), &req, &err) == -1);
    assert(err == 400);

    /* 본문 상한 1MiB + 1 초과 -> 413 */
    const char *over_body =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 1048577\r\n"
        "\r\n";
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_head(over_body, strlen(over_body), &req, &err) == -1);
    assert(err == 413);

    /* Transfer-Encoding 단독 -> 501 */
    const char *te_only =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_head(te_only, strlen(te_only), &req, &err) == -1);
    assert(err == 501);

    /* 각종 400 거부 케이스 */
    const char *bad_heads[] = {
        "\r\nGET /hello HTTP/1.1\r\nHost: a\r\n\r\n",
        "GET /hello HTTP/1.1\nHost: a\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\nX-A: 1\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\rX-A: 1\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: \r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 12abc\r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: \r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 10\r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: \r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
        "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Type: text/plain\r\nContent-Type: text/html\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad_heads) / sizeof(bad_heads[0]); i++) {
        memset(&req, 0, sizeof(req));
        err = HTTP_STATUS_NONE;
        assert(parse_request_head(bad_heads[i], strlen(bad_heads[i]), &req, &err) == -1);
        assert(err == 400);
    }

    /* NUL 바이트 및 RFC 7230 불허 제어 문자·비-ASCII 포함 헤드 -> 400 */
    const char nul_head[] = "GET /hello HTTP/1.1\r\nHost: a\0\r\n\r\n";
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_head(nul_head, sizeof(nul_head) - 1, &req, &err) == -1);
    assert(err == 400);

    const char *ctl_heads[] = {
        "GET /hello HTTP/1.1\r\nHost: a\r\nX-Test: \x01" "v\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\r\nX-Test: \x0b" "v\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\r\nX-Test: \x0c" "v\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\r\nX-Test: \x1b" "v\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\r\nX-Test: \x7f" "v\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\r\nX-Test: \x80" "v\r\n\r\n",
        "GET /hello HTTP/1.1\r\nHost: a\r\nX-Test: \xff" "v\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(ctl_heads) / sizeof(ctl_heads[0]); i++) {
        memset(&req, 0, sizeof(req));
        err = HTTP_STATUS_NONE;
        assert(parse_request_head(ctl_heads[i], strlen(ctl_heads[i]), &req, &err) == -1);
        assert(err == HTTP_STATUS_BAD_REQUEST);
    }

    /* Host 260B 초과 -> 400 */
    char long_host[350];
    snprintf(long_host, sizeof(long_host), "GET /hello HTTP/1.1\r\nHost: ");
    size_t pos = strlen(long_host);
    memset(long_host + pos, 'a', 260);
    memcpy(long_host + pos + 260, "\r\n\r\n", 4);
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_head(long_host, pos + 264, &req, &err) == -1);
    assert(err == 400);

    /* Content-Type 256B 초과 -> 400 */
    char long_ct[350];
    snprintf(long_ct, sizeof(long_ct), "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Type: ");
    pos = strlen(long_ct);
    memset(long_ct + pos, 'b', 256);
    memcpy(long_ct + pos + 256, "\r\n\r\n", 4);
    memset(&req, 0, sizeof(req));
    err = HTTP_STATUS_NONE;
    assert(parse_request_head(long_ct, pos + 260, &req, &err) == -1);
    assert(err == 400);
}

int main(void) {
    test_request_line_valid();
    test_request_line_invalid();
    test_header_line_syntax();
    test_request_head_valid_and_errors();
    printf("[PASS] test_parse_request\n");
    return 0;
}
