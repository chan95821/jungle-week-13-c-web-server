#define _POSIX_C_SOURCE 200809L
#include "../../src/proxy.c"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>

/* 헤더 완료 전에 연결이 끝나거나 수신 상한이 소진되면 백엔드 오류를 반환한다. */
static void rejects_unfinished_backend_headers(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(write(pair[1], "HTTP/1.1 200", 12) == 12);
    assert(shutdown(pair[1], SHUT_WR) == 0);
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 502);
    close(pair[0]);
    close(pair[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    char oversized[REQ_HEADER_MAX_BYTES];
    memset(oversized, 'x', sizeof(oversized));
    assert(send_all_bytes(pair[1], oversized, sizeof(oversized), NULL) == 0);
    assert(receive_proxy_response(pair[0], &request, &response) == 502);
    close(pair[0]);
    close(pair[1]);
}

/* 백엔드 무응답이면 최종 응답 송신 전에 504로 돌아올 수 있어야 한다. */
static void bounds_backend_header_wait(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 504);
    close(pair[0]);
    close(pair[1]);
}

/* 수신한 헤더의 길이 선언 오류가 실제 응답 준비 경로에서도 502로 연결되는지 확인한다. */
static void rejects_invalid_response_framing(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const unsigned char wire[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n";
    assert(write(pair[1], wire, sizeof(wire) - 1) == (ssize_t)(sizeof(wire) - 1));
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 502);
    assert(fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]);
    close(pair[1]);
}

/* 실제 로컬 소켓으로 응답 헤더 단계의 오류를 확인한다. */
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    rejects_unfinished_backend_headers();
    bounds_backend_header_wait();
    rejects_invalid_response_framing();
    puts("[PASS] test_proxy_receive");
    return 0;
}
