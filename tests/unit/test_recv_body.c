#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../src/server.h"

static void test_zero_length(void) {
    uint8_t *body = (uint8_t *)0x1;
    size_t len = 99;

    assert(recv_body(-1, NULL, 0, 0, &body, &len) == 0);
    assert(body == NULL);
    assert(len == 0);
}

static void test_excess_covers_all(void) {
    uint8_t *body = NULL;
    size_t len = 0;
    const char *excess = "hello world";

    assert(recv_body(-1, excess, strlen(excess), 5, &body, &len) == 0);
    assert(len == 5);
    assert(body != NULL);
    assert(memcmp(body, "hello", 5) == 0);
    free(body);
}

static void test_split_recv_and_binary(void) {
    int sv[2];
    uint8_t *body = NULL;
    size_t len = 0;
    const uint8_t excess[3] = {'A', '\0', 'B'};
    const uint8_t part1[4] = {'C', 0xFF, '\0', 'D'};
    const uint8_t part2[3] = {'E', 'F', '\0'};

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        send_all(sv[1], part1, sizeof(part1));
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10000000L}, NULL);
        send_all(sv[1], part2, sizeof(part2));
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);

    assert(recv_body(sv[0], (const char *)excess, sizeof(excess), 10, &body, &len) == 0);
    assert(len == 10);
    assert(memcmp(body, excess, 3) == 0);
    assert(memcmp(body + 3, part1, 4) == 0);
    assert(memcmp(body + 7, part2, 3) == 0);
    free(body);

    close(sv[0]);
    waitpid(pid, NULL, 0);
}

static void test_early_eof_no_leak(void) {
    int sv[2];
    uint8_t *body = NULL;
    size_t len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send(sv[1], "ab", 2, 0) == 2);
    close(sv[1]);

    assert(recv_body(sv[0], NULL, 0, 5, &body, &len) == -1);
    assert(body == NULL);
    close(sv[0]);
}

int main(void) {
    test_zero_length();
    test_excess_covers_all();
    test_split_recv_and_binary();
    test_early_eof_no_leak();
    printf("[PASS] test_recv_body\n");
    return 0;
}
