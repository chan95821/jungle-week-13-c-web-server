#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../src/server.h"

static void test_header_and_body_lengths(void) {
    int sv[2];
    char buffer[REQ_HEADER_MAX_BYTES];
    size_t total = 0, header = 0;
    const char data[] = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send_all(sv[1], data, sizeof(data) - 1) == 0);
    assert(shutdown(sv[1], SHUT_WR) == 0);
    assert(recv_request_headers(sv[0], buffer, sizeof(buffer), &total, &header) == 0);
    assert(header == sizeof(data) - 1 - 5);
    struct http_request request;
    assert(parse_http_request(buffer, header, &request) == 0);
    assert(recv_request_body(sv[0], &request, buffer + header, total - header) == 0);
    assert(request.body_length == 5 && memcmp(request.body, "hello", 5) == 0);
    free(request.body);
    close(sv[0]);
    close(sv[1]);
}

static void test_every_initial_body_split(void) {
    /* 이미 받은 본문의 길이를 0..전체 길이로 바꾼다. 실제 recv의 분할 시점에
     * 의존하지 않고 모든 initial/remaining 조합을 결정적으로 검사한다. */
    const unsigned char body[] = {0, 255, 'h', '\r', '\n', '\r', '\n', 'i'};
    for (size_t split = 0; split <= sizeof(body); ++split) {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        struct http_request request = {0};
        request.content_length = sizeof(body);
        assert(send_all(sv[1], body + split, sizeof(body) - split) == 0);
        assert(recv_request_body(sv[0], &request, body, split) == 0);
        assert(request.body_length == sizeof(body));
        assert(memcmp(request.body, body, sizeof(body)) == 0);
        free(request.body);
        close(sv[0]);
        close(sv[1]);
    }
}

static void test_incomplete_and_extra(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct http_request request = {0};
    request.content_length = 5;
    assert(shutdown(sv[1], SHUT_WR) == 0);
    assert(recv_request_body(sv[0], &request, "hel", 3) == 400);
    assert(request.body_length == 3);
    free(request.body); /* 실패 경로에서도 소유자는 호출자다. */
    close(sv[0]);
    close(sv[1]);

    memset(&request, 0, sizeof(request));
    request.content_length = 5;
    assert(recv_request_body(-1, &request, "helloEXTRA", 10) == 0);
    assert(request.body_length == 5 && memcmp(request.body, "hello", 5) == 0);
    free(request.body);
    memset(&request, 0, sizeof(request));
    assert(recv_request_body(-1, &request, NULL, 0) == 0);
    assert(request.body == NULL && request.body_length == 0);
    request.content_length = REQ_BODY_MAX_BYTES + 1;
    assert(recv_request_body(-1, &request, NULL, 0) == 413);
    assert(request.body == NULL);
}

static void test_timeout(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct timeval timeout = {0, 20000};
    assert(setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    struct http_request request = {0};
    request.content_length = 1;
    assert(recv_request_body(sv[0], &request, NULL, 0) == 408);
    free(request.body);
    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    test_header_and_body_lengths();
    test_every_initial_body_split();
    test_incomplete_and_extra();
    test_timeout();
    puts("[PASS] test_recv_body");
    return 0;
}
