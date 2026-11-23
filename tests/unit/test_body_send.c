#define _POSIX_C_SOURCE 200809L

/* Exercise the internal sender without adding a production API. */
#include "../../src/server.c"

#include <assert.h>

struct test_body {
    enum body_result result;
    const unsigned char *data;
    size_t length;
    int calls;
    int error;
};

static enum body_result test_next(void *state, struct http_slice *chunk) {
    struct test_body *body = state;
    body->calls++;
    chunk->data = body->data;
    chunk->length = body->length;
    errno = body->error;
    return body->result;
}

static void check_failure(enum body_result result, const unsigned char *data,
                          size_t chunk_length, size_t expected, int error) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct test_body body = {result, data, chunk_length, 0, error};
    struct body_provider provider = {.next_func = test_next, .state = &body};
    assert(send_body(sv[0], &provider, expected, NULL) == -1);
    assert(body.calls == 1);
    assert(errno == (error ? error : EIO));
    assert(shutdown(sv[0], SHUT_WR) == 0);
    char byte;
    assert(recv(sv[1], &byte, 1, 0) == 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_provider_failures(void) {
    static const unsigned char data[] = "abc";
    check_failure(BODY_END, NULL, 0, 3, 0);
    check_failure(BODY_ERROR, NULL, 0, 3, 0);
    check_failure(BODY_ERROR, NULL, 0, 3, ENOSPC);
    check_failure(BODY_DATA, NULL, 1, 3, 0);
    check_failure(BODY_DATA, data, 0, 3, 0);
    check_failure(BODY_DATA, data, 3, 2, 0);
}

static void test_length_and_send_failure(void) {
    static const unsigned char data[] = "abc";
    struct test_body body = {BODY_DATA, data, 3, 0, 0};
    struct body_provider provider = {.next_func = test_next, .state = &body};
    assert(send_body(-1, &provider, 0, NULL) == 0);
    assert(body.calls == 0);

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send_body(sv[0], &provider, 3, NULL) == 0);
    assert(body.calls == 1);
    unsigned char received[3];
    assert(recv(sv[1], received, sizeof(received), MSG_WAITALL) == 3);
    assert(memcmp(received, data, 3) == 0);
    close(sv[1]);
    assert(send_body(sv[0], &provider, 3, NULL) == -1);
    assert(body.calls == 2);
    close(sv[0]);
}

struct response_test_body {
    enum body_result result;
    const unsigned char *data;
    size_t length;
    int calls;
    int cleanups;
    int peer;
    int close_peer;
};

static enum body_result response_test_next(void *state, struct http_slice *chunk) {
    struct response_test_body *body = state;
    body->calls++;
    if (body->close_peer) {
        char header[512];
        ssize_t n = recv(body->peer, header, sizeof(header) - 1, MSG_DONTWAIT);
        assert(n > 0);
        header[n] = '\0';
        assert(strstr(header, "\r\n\r\n") != NULL);
        close(body->peer);
        body->peer = -1;
    }
    chunk->data = body->data;
    chunk->length = body->length;
    return body->result;
}

static void response_test_cleanup(void *state) {
    struct response_test_body *body = state;
    body->cleanups++;
    errno = EDOM;
}

static void test_response_cleanup(void) {
    static const unsigned char bytes[] = "abc";
    struct {
        enum body_result result;
        const unsigned char *data;
        size_t chunk_length, declared_length;
        int close_before_header, close_in_next, cleanup, expected_result, calls;
    } cases[] = {
        {BODY_DATA, bytes, 3, 3, 0, 0, 1, 0, 1},
        {BODY_DATA, bytes, 3, 0, 0, 0, 1, 0, 0},
        {BODY_DATA, bytes, 3, 3, 1, 0, 1, -1, 0},
        {BODY_DATA, bytes, 3, 3, 0, 1, 1, -1, 1},
        {BODY_END, NULL, 0, 3, 0, 0, 1, -1, 1},
        {BODY_ERROR, NULL, 0, 3, 0, 0, 1, -1, 1},
        {BODY_DATA, NULL, 1, 3, 0, 0, 1, -1, 1},
        {BODY_DATA, bytes, 0, 3, 0, 0, 1, -1, 1},
        {BODY_DATA, bytes, 3, 2, 0, 0, 1, -1, 1},
        {BODY_DATA, bytes, 3, 3, 0, 0, 0, 0, 1},
        {BODY_END, NULL, 0, 3, 0, 0, 0, -1, 1}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        struct response_test_body body = {
            .result = cases[i].result, .data = cases[i].data,
            .length = cases[i].chunk_length, .peer = sv[1],
            .close_peer = cases[i].close_in_next
        };
        struct router_response response = {
            .status = 200, .content_type = "text/plain",
            .length = cases[i].declared_length,
            .body_provider = {
                .next_func = response_test_next,
                .cleanup_func = cases[i].cleanup ? response_test_cleanup : NULL,
                .state = &body
            }
        };
        if (cases[i].close_before_header) {
            close(sv[1]);
            body.peer = -1;
        }
        int result = send_prepared_response(sv[0], &response, false, NULL);
        int saved_errno = errno;
        assert(result == cases[i].expected_result);
        assert(body.calls == cases[i].calls);
        assert(body.cleanups == cases[i].cleanup);
        if (result < 0) assert(saved_errno != EDOM);
        if (result < 0 && !cases[i].close_before_header && !cases[i].close_in_next)
            assert(saved_errno == EIO);
        if (body.peer >= 0) {
            assert(shutdown(sv[0], SHUT_WR) == 0);
            char wire[1024];
            size_t used = 0;
            ssize_t n;
            while ((n = recv(body.peer, wire + used, sizeof(wire) - 1 - used, 0)) > 0)
                used += (size_t)n;
            assert(n == 0);
            wire[used] = '\0';
            const char *end = strstr(wire, "\r\n\r\n");
            assert(end != NULL);
            size_t payload = used - (size_t)(end + 4 - wire);
            assert(payload == (result == 0 ? cases[i].declared_length : 0));
            if (payload) assert(memcmp(end + 4, bytes, payload) == 0);
            close(body.peer);
        }
        close(sv[0]);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_provider_failures();
    test_length_and_send_failure();
    test_response_cleanup();
    puts("[PASS] test_body_send");
    return 0;
}
