#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../src/server.h"

static void test_zero_length(void) {
    assert(send_all(-1, "abc", 0) == 0);
}

static void test_partial_send(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    int sndbuf = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    const size_t total_sz = 65536;
    uint8_t *payload = malloc(total_sz);
    uint8_t *received = malloc(total_sz);
    assert(payload != NULL && received != NULL);
    for (size_t i = 0; i < total_sz; i++) {
        payload[i] = (uint8_t)(i & 0xff);
    }

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[1]);
        int rc = send_all(sv[0], payload, total_sz);
        close(sv[0]);
        free(payload);
        free(received);
        _exit(rc == 0 ? 0 : 1);
    }
    close(sv[0]);

    size_t got = 0;
    while (got < total_sz) {
        ssize_t n = recv(sv[1], received + got, total_sz - got, 0);
        assert(n > 0);
        got += (size_t)n;
    }
    close(sv[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(got == total_sz);
    assert(memcmp(payload, received, total_sz) == 0);
    free(payload);
    free(received);
}

static void test_closed_peer(void) {
    int sv[2];

    signal(SIGPIPE, SIG_IGN);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    assert(send_all(sv[0], "hello", 5) == -1);
    close(sv[0]);
}

static void test_vector_validation_and_empty(void) {
    struct iovec one = {.iov_base = (void *)"a", .iov_len = 1};
    struct iovec too_many[17];
    for (size_t i = 0; i < 17; ++i) too_many[i] = one;

    errno = 0;
    assert(send_allv_with_deadline(-1, NULL, 1, NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(send_allv_with_deadline(-1, &one, 0, NULL) == -1);
    assert(errno == EINVAL);

    errno = 0;
    assert(send_allv_with_deadline(-1, too_many, 17, NULL) == -1);
    assert(errno == EINVAL);

    struct iovec null_data = {.iov_base = NULL, .iov_len = 4};
    errno = 0;
    assert(send_allv_with_deadline(-1, &null_data, 1, NULL) == -1);
    assert(errno == EINVAL);

    struct iovec empty[2] = {
        {.iov_base = NULL, .iov_len = 0},
        {.iov_base = (void *)"ignored", .iov_len = 0}
    };
    assert(send_allv_with_deadline(-1, empty, 2, NULL) == 0);
}

static void test_vector_partial_writev(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    int sndbuf = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    const char prefix[] = "10000\r\n";
    const char suffix[] = "\r\n";
    const size_t body_sz = 65536;
    const size_t total_sz = (sizeof(prefix) - 1) + body_sz + (sizeof(suffix) - 1);

    uint8_t *payload = malloc(body_sz);
    uint8_t *received = malloc(total_sz);
    assert(payload != NULL && received != NULL);
    for (size_t i = 0; i < body_sz; ++i) {
        payload[i] = (uint8_t)(i & 0xff);
    }

    const struct iovec vectors[4] = {
        {.iov_base = (void *)prefix, .iov_len = sizeof(prefix) - 1},
        {.iov_base = NULL, .iov_len = 0},
        {.iov_base = payload, .iov_len = body_sz},
        {.iov_base = (void *)suffix, .iov_len = sizeof(suffix) - 1}
    };

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[1]);
        int rc = send_allv_with_deadline(sv[0], vectors, 4, NULL);
        int ok = (rc == 0 && vectors[2].iov_base == payload && vectors[2].iov_len == body_sz);
        close(sv[0]);
        free(payload);
        free(received);
        _exit(ok ? 0 : 1);
    }
    close(sv[0]);

    size_t got = 0;
    while (got < total_sz) {
        ssize_t n = recv(sv[1], received + got, total_sz - got, 0);
        assert(n > 0);
        got += (size_t)n;
    }
    close(sv[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(got == total_sz);
    assert(memcmp(received, prefix, sizeof(prefix) - 1) == 0);
    assert(memcmp(received + (sizeof(prefix) - 1), payload, body_sz) == 0);
    assert(memcmp(received + (sizeof(prefix) - 1) + body_sz, suffix, sizeof(suffix) - 1) == 0);
    free(payload);
    free(received);
}

static void test_vector_deadline_and_closed_peer(void) {
    int sv[2];
    signal(SIGPIPE, SIG_IGN);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    int sndbuf = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    assert(socket_configure_timeouts(sv[0]) == 0);

    const size_t huge_sz = 256 * 1024;
    uint8_t *huge = calloc(1, huge_sz);
    assert(huge != NULL);

    struct timespec deadline;
    assert(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
    deadline.tv_nsec += 120000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    struct io_context io = {.cancel = NULL, .deadline = &deadline};
    struct iovec vecs[2] = {
        {.iov_base = (void *)"HDR\r\n", .iov_len = 5},
        {.iov_base = huge, .iov_len = huge_sz}
    };
    errno = 0;
    assert(send_allv_with_deadline(sv[0], vecs, 2, &io) == -1);
    assert(errno == ETIMEDOUT);

    close(sv[1]);
    assert(send_allv_with_deadline(sv[0], vecs, 1, NULL) == -1);
    close(sv[0]);
    free(huge);
}

int main(void) {
    test_zero_length();
    test_partial_send();
    test_closed_peer();
    test_vector_validation_and_empty();
    test_vector_partial_writev();
    test_vector_deadline_and_closed_peer();
    printf("[PASS] test_send_all\n");
    return 0;
}
