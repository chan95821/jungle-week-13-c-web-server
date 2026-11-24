#define _POSIX_C_SOURCE 200809L
/* Existing unit-test convention: inspect internal orchestration without exporting it. */
#include "../../src/proxy.c"
#include "../../src/proxy_selection.h"

#include <assert.h>
#include <sys/socket.h>

static size_t call_count;
static uint16_t attempted_ports[2];
static int outcomes[2];
static int peer_fd = -1;
static size_t reported_backend;
static int reported_status;

/* 결과 기록 실패를 재현해 연결 성공 후에도 호출자가 FD 소유권을 누수하지 않는지 검사한다. */
static int fail_to_record_connection(struct proxy_group_state *group, size_t selected, int status) {
    (void)group;
    reported_backend = selected;
    reported_status = status;
    return 500;
}

/* Replace only one-server connection; successful cases return an owned real FD. */
int connect_backend(const struct proxy_backend *backend, int *backend_fd) {
    assert(call_count < 2);
    attempted_ports[call_count] = backend->port;
    int result = outcomes[call_count++];
    *backend_fd = -1;
    if (result == 0) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        *backend_fd = pair[0];
        peer_fd = pair[1];
    }
    return result;
}

/* Reset observations before each acquisition, independently of group state. */
static void reset_connection_results(int first, int second) {
    assert(peer_fd == -1);
    call_count = 0;
    outcomes[0] = first;
    outcomes[1] = second;
}

/* Verify caller ownership and absence of HTTP bytes at the acquisition boundary. */
static void release_test_connection(int fd) {
    assert(fd >= 0 && peer_fd >= 0);
    assert(close(fd) == 0);
    char byte;
    assert(recv(peer_fd, &byte, 1, 0) == 0);
    assert(close(peer_fd) == 0);
    peer_fd = -1;
}

/* Verify the fixed backend is attempted once and internal failures are not retried. */
static void fixed_backend_attempt(void) {
    const struct proxy_backend backend = {"127.0.0.1", 8001};
    const struct proxy_upstream upstream = {
        .backends = &backend, .backend_count = 1, .max_attempts = 1
    };
    int fd;
    reset_connection_results(502, 0);
    assert(acquire_proxy_backend(&upstream, &fd) == 502);
    assert(fd == -1 && call_count == 1 && attempted_ports[0] == 8001);

    reset_connection_results(0, 0);
    assert(acquire_proxy_backend(&upstream, &fd) == 0);
    assert(call_count == 1);
    release_test_connection(fd);
}

/* Verify fallback, temporary exclusion, request-based recovery and all-unavailable handling. */
static void group_failure_and_recovery(void) {
    const struct proxy_backend backends[] = {{"127.0.0.1", 8002}, {"127.0.0.1", 8003}};
    struct timespec excluded[2] = {0};
    struct proxy_group_state group = {.excluded_until = excluded};
    const struct proxy_upstream upstream = {
        .backends = backends, .backend_count = 2, .max_attempts = 2,
        .select_backend = select_available_round_robin_backend,
        .on_connection_result = record_backend_connection_result, .group_state = &group
    };
    int fd;
    reset_connection_results(502, 0);
    assert(acquire_proxy_backend(&upstream, &fd) == 0);
    assert(call_count == 2 && attempted_ports[0] == 8002 && attempted_ports[1] == 8003);
    assert(excluded[0].tv_sec != 0);
    release_test_connection(fd);

    /* Use explicit future/past instants rather than sleeping near an expiry boundary. */
    assert(clock_gettime(CLOCK_MONOTONIC, &excluded[0]) == 0);
    excluded[0].tv_sec += 60;
    group.next = 0;
    reset_connection_results(0, 0);
    assert(acquire_proxy_backend(&upstream, &fd) == 0);
    assert(call_count == 1 && attempted_ports[0] == 8003);
    release_test_connection(fd);

    excluded[1] = excluded[0];
    reset_connection_results(0, 0);
    assert(acquire_proxy_backend(&upstream, &fd) == 502);
    assert(fd == -1 && call_count == 0);

    excluded[0] = (struct timespec){.tv_sec = 1};
    group.next = 0;
    reset_connection_results(0, 0);
    assert(acquire_proxy_backend(&upstream, &fd) == 0);
    assert(call_count == 1 && attempted_ports[0] == 8002);
    assert(excluded[0].tv_sec == 0 && excluded[0].tv_nsec == 0);
    release_test_connection(fd);

    excluded[0] = excluded[1] = (struct timespec){0};
    reset_connection_results(502, 502);
    assert(acquire_proxy_backend(&upstream, &fd) == 502);
    assert(fd == -1 && call_count == 2 && attempted_ports[0] != attempted_ports[1]);

    excluded[0] = excluded[1] = (struct timespec){0};
    reset_connection_results(500, 0);
    assert(acquire_proxy_backend(&upstream, &fd) == 500);
    assert(call_count == 1 && excluded[0].tv_sec == 0 && excluded[1].tv_sec == 0);

    struct proxy_upstream broken_recorder = upstream;
    broken_recorder.on_connection_result = fail_to_record_connection;
    group.next = 0;
    reset_connection_results(0, 0);
    assert(acquire_proxy_backend(&broken_recorder, &fd) == 500);
    assert(fd == -1 && call_count == 1 && reported_backend == 0 && reported_status == 0);
    char byte;
    assert(recv(peer_fd, &byte, 1, 0) == 0);
    assert(close(peer_fd) == 0);
    peer_fd = -1;
}

/* Check acquisition control flow while leaving actual TCP implementation out of scope. */
int main(void) {
    fixed_backend_attempt();
    group_failure_and_recovery();
    puts("[PASS] test_proxy_acquire");
    return 0;
}
