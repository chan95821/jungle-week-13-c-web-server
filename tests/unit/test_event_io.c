#define _GNU_SOURCE
#include "../../src/event_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 작은 커널 송신 버퍼로 실제 partial write와 EAGAIN을 강제한다.
 * 다시 호출했을 때 prefix/body/suffix 경계와 NUL 바이트가 정확히 보존되어야 한다. */
static void check_partial_output(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    int capacity = 4096;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    size_t body_length = 256 * 1024;
    unsigned char *body = malloc(body_length);
    unsigned char *received = malloc(body_length + 6);
    assert(body && received);
    for (size_t i = 0; i < body_length; ++i) body[i] = (unsigned char)i;
    struct event_connection connection = {.phase = SEND_CLIENT_BODY};
    struct event_endpoint endpoint = {.fd = pair[0]};
    set_connection_output(&connection, (struct http_slice){(const unsigned char *)"P\0FX", 4},
                            (struct http_slice){body, body_length},
                            (struct http_slice){(const unsigned char *)"\r\n", 2});
    int result;
    do { result = send_pending_output(&connection, &endpoint); } while (result == 2);
    assert(result == 0); /* 데이터를 아직 읽지 않았으므로 EAGAIN이어야 한다. */
    assert(connection.response_started && connection.output.offset > 0);
    size_t total = 0;
    for (unsigned int step = 0; step < 10000 && total < body_length + 6; ++step) {
        ssize_t count = recv(pair[1], received + total, body_length + 6 - total, 0);
        if (count > 0) total += (size_t)count;
        else assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        result = send_pending_output(&connection, &endpoint);
        assert(result >= 0);
    }
    assert(total == body_length + 6 && result == 1);
    assert(!memcmp(received, "P\0FX", 4));
    assert(!memcmp(received + 4, body, body_length));
    assert(!memcmp(received + 4 + body_length, "\r\n", 2));
    free(body); free(received);
    close(pair[0]); close(pair[1]);
}

static void check_deadline_heap(void) {
    struct event_worker worker = {0};
    struct event_connection connections[4] = {0};
    const int64_t deadlines[] = {40, 10, 30, 20};
    for (size_t i = 0; i < 4; ++i) {
        connections[i].worker = &worker;
        connections[i].heap_index = SIZE_MAX;
        connections[i].phase = READ_CLIENT_HEADER;
        connections[i].stage_deadline = deadlines[i];
        connections[i].client_idle_deadline = 100;
        update_connection_timer(&connections[i]);
    }
    assert(worker.timers[0] == &connections[1]);
    connections[0].stage_deadline = 5;
    update_connection_timer(&connections[0]);
    assert(worker.timers[0] == &connections[0]);
    connections[0].stage_deadline = 50;
    update_connection_timer(&connections[0]);
    assert(worker.timers[0] == &connections[1]);
    int64_t previous = 0;
    while (worker.timer_count) {
        struct event_connection *first = worker.timers[0];
        assert(first->timer_at >= previous);
        previous = first->timer_at;
        forget_connection_timer(first);
        assert(first->heap_index == SIZE_MAX);
    }
}

int main(void) {
    check_partial_output();
    check_deadline_heap();
    puts("event I/O: partial output and deadline heap passed");
    return 0;
}
