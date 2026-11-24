#define _POSIX_C_SOURCE 200809L
/* 기존 white-box 방식으로 정적 본문 공급자 조율을 검사한다. */
#include "../../src/proxy.c"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>

/* 공급자가 돌려준 바이트를 모아 선언 길이 도달 여부와 내용을 확인한다. */
static enum body_result collect(struct router_response *response,
                                unsigned char *out, size_t capacity, size_t *length) {
    *length = 0;
    for (;;) {
        struct http_slice chunk = {0};
        enum body_result result = response->body_provider.next_func(
            response->body_provider.state, &chunk);
        if (result != BODY_DATA) return result;
        assert(chunk.data && chunk.length > 0);
        assert(*length + chunk.length <= capacity);
        memcpy(out + *length, chunk.data, chunk.length);
        *length += chunk.length;
    }
}

/* 헤더와 함께 읽힌 본문을 응답 소유 저장소로 옮겨 손실 없이 제공하는지 확인한다. */
static void relays_prefetched_body(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char wire[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 5\r\n\r\nhello";
    assert(write(pair[1], wire, sizeof(wire) - 1) == (ssize_t)(sizeof(wire) - 1));
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 0);
    assert(response.status == 200 && response.length == 5);
    assert(strcmp(response.content_type, "text/plain") == 0);

    unsigned char output[16];
    size_t length;
    assert(collect(&response, output, sizeof(output), &length) == BODY_END);
    assert(length == 5 && memcmp(output, "hello", 5) == 0);
    response.body_provider.cleanup_func(response.body_provider.state);
    assert(fcntl(pair[0], F_GETFD) == -1);
    close(pair[1]);
}

/* 헤더 뒤에 나뉘어 도착한 본문도 이어 읽고, 여분 바이트는 중계하지 않는다. */
static void reads_split_body_without_extra(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char headers[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    assert(write(pair[1], headers, sizeof(headers) - 1) == (ssize_t)(sizeof(headers) - 1));
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 0);
    assert(strcmp(response.content_type, "application/octet-stream") == 0);
    assert(write(pair[1], "hel", 3) == 3);
    assert(write(pair[1], "loEXTRA", 7) == 7);

    unsigned char output[16];
    size_t length;
    assert(collect(&response, output, sizeof(output), &length) == BODY_END);
    assert(length == 5 && memcmp(output, "hello", 5) == 0);
    response.body_provider.cleanup_func(response.body_provider.state);
    close(pair[1]);
}

/* 선언 길이에 도달하기 전에 백엔드가 끝나면 오류를 반환해 불완전 응답을 완료로 오인하지 않는다. */
static void reports_truncated_body(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char wire[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 10\r\n\r\nabc";
    assert(write(pair[1], wire, sizeof(wire) - 1) == (ssize_t)(sizeof(wire) - 1));
    assert(shutdown(pair[1], SHUT_WR) == 0);
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 0);
    assert(response.length == 10);

    unsigned char output[16];
    size_t length;
    assert(collect(&response, output, sizeof(output), &length) == BODY_ERROR);
    assert(length == 3 && memcmp(output, "abc", 3) == 0);
    response.body_provider.cleanup_func(response.body_provider.state);
    close(pair[1]);
}

/* 본문 없는 응답은 읽기 없이 길이 0으로 준비하고 정리에서 FD를 닫는다. */
static void prepares_bodiless_response(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char wire[] = "HTTP/1.1 204 No Content\r\n\r\n";
    assert(write(pair[1], wire, sizeof(wire) - 1) == (ssize_t)(sizeof(wire) - 1));
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 0);
    assert(response.status == 204 && response.length == 0);
    struct http_slice chunk = {0};
    assert(response.body_provider.next_func(response.body_provider.state, &chunk) == BODY_END);
    response.body_provider.cleanup_func(response.body_provider.state);
    assert(fcntl(pair[0], F_GETFD) == -1);
    close(pair[1]);
}

/* 전달 대상 응답 헤더만 순서·다중 줄 그대로 보존하고 hop-by-hop·재작성 필드는 뺀다. */
static void selects_forwarded_response_headers(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char wire[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: /next\r\n"
        "Set-Cookie: a=1\r\n"
        "Set-Cookie: b=2\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close, X-Drop\r\n"
        "X-Drop: secret\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 2\r\n\r\nok";
    assert(write(pair[1], wire, sizeof(wire) - 1) == (ssize_t)(sizeof(wire) - 1));
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 0);
    assert(response.status == 302 && response.headers != NULL && response.headers_length > 0);
    assert(strcmp(response.content_type, "text/plain") == 0);

    static const char expected[] =
        "Location: /next\r\n"
        "Set-Cookie: a=1\r\n"
        "Set-Cookie: b=2\r\n"
        "Cache-Control: no-store\r\n";
    assert(response.headers_length == sizeof(expected) - 1);
    assert(memcmp(response.headers, expected, sizeof(expected) - 1) == 0);

    response.body_provider.cleanup_func(response.body_provider.state);
    close(pair[1]);
}

/* 최종 1xx를 그대로 중계하지 않도록 아직은 거부하며 FD를 호출자가 정리한다. */
static void rejects_interim_response_for_now(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const char wire[] = "HTTP/1.1 103 Early Hints\r\n\r\n";
    assert(write(pair[1], wire, sizeof(wire) - 1) == (ssize_t)(sizeof(wire) - 1));
    struct http_request request = {0};
    struct router_response response;
    assert(receive_proxy_response(pair[0], &request, &response) == 502);
    assert(fcntl(pair[0], F_GETFD) >= 0);
    close(pair[0]);
    close(pair[1]);
}

/* 본문 공급자의 바이트 손실·조기 완료·FD 종료를 실제 소켓으로 확인한다. */
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    relays_prefetched_body();
    reads_split_body_without_extra();
    reports_truncated_body();
    prepares_bodiless_response();
    selects_forwarded_response_headers();
    rejects_interim_response_for_now();
    puts("[PASS] test_proxy_body");
    return 0;
}
