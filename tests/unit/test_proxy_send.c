#define _POSIX_C_SOURCE 200809L
/* 기존 white-box 방식으로 정적 조율 함수를 검사한다. */
#include "../../src/proxy.c"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

/* 작은 송신 버퍼로 부분 송신을 유도하고 실제 수신 바이트가 헤더 뒤 본문인지 확인한다. */
static void sends_headers_then_binary_body(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int small_buffer = 1024;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)) == 0);
    unsigned char body[REQ_BODY_MAX_BYTES];
    for (size_t i = 0; i < sizeof(body); ++i) body[i] = (unsigned char)i;
    const unsigned char header[] = "POST / HTTP/1.1\r\nContent-Length: 65536\r\n\r\n";
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pair[0]);
        unsigned char received[sizeof(header) - 1 + sizeof(body)];
        size_t offset = 0;
        while (offset < sizeof(received)) {
            ssize_t count = recv(pair[1], received + offset, sizeof(received) - offset, 0);
            if (count <= 0) _exit(1);
            offset += (size_t)count;
        }
        bool equal = memcmp(received, header, sizeof(header) - 1) == 0 &&
                     memcmp(received + sizeof(header) - 1, body, sizeof(body)) == 0;
        close(pair[1]);
        _exit(equal ? 0 : 1);
    }
    close(pair[1]);
    assert(send_proxy_request(pair[0], (struct http_slice){header, sizeof(header) - 1},
                              (struct http_slice){body, sizeof(body)}) == 0);
    assert(fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* 상대가 읽지 않거나 종료해도 함수는 FD를 닫지 않고 오류만 반환한다. */
static void returns_errors_without_closing_fd(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const unsigned char header[] = "GET / HTTP/1.1\r\nHost: example\r\n\r\n";
    struct http_slice headers = {header, sizeof(header) - 1};
    assert(send_proxy_request(pair[0], headers, (struct http_slice){0}) == 0);
    unsigned char received[sizeof(header) - 1];
    assert(recv(pair[1], received, sizeof(received), MSG_WAITALL) == (ssize_t)sizeof(received));
    assert(memcmp(received, header, sizeof(received)) == 0);

    int small_buffer = 1024;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)) == 0);
    unsigned char body[REQ_BODY_MAX_BYTES] = {0};
    assert(send_proxy_request(pair[0], headers, (struct http_slice){body, sizeof(body)}) == 504);
    assert(fcntl(pair[0], F_GETFD) >= 0);
    close(pair[1]);
    assert(send_proxy_request(pair[0], headers, (struct http_slice){0}) == 502);
    assert(fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]);
}

/* 서버와 같이 SIGPIPE를 무시하고 부분 송신과 오류 반환 계약을 확인한다. */
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    sends_headers_then_binary_body();
    returns_errors_without_closing_fd();
    puts("[PASS] test_proxy_send");
    return 0;
}
