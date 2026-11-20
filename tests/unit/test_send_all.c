#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

int main(void) {
    test_zero_length();
    test_partial_send();
    test_closed_peer();
    printf("[PASS] test_send_all\n");
    return 0;
}
