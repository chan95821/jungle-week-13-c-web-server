#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../src/server.h"

static void test_single_recv(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *req = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    assert(send(sv[1], req, strlen(req), 0) == (ssize_t)strlen(req));
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == 0);
    assert(header_len == strlen(req));
    assert(total_len == strlen(req));
    close(sv[0]);
    close(sv[1]);
}

static void test_split_boundary(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *part1 = "GET / HTTP/1.1\r\nHost: a\r\n\r";
    const char *part2 = "\n";
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        send(sv[1], part1, strlen(part1), 0);
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10000000L}, NULL);
        send(sv[1], part2, strlen(part2), 0);
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == 0);
    assert(header_len == strlen(part1) + strlen(part2));
    assert(total_len == header_len);
    close(sv[0]);
    waitpid(pid, NULL, 0);
}

static void test_with_trailing_body(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *head = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n";
    const char *req = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello";
    assert(send(sv[1], req, strlen(req), 0) == (ssize_t)strlen(req));
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == 0);
    assert(header_len == strlen(head));
    assert(total_len == strlen(req));
    assert(memcmp(buf + header_len, "hello", 5) == 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_header_plus_10kb_body(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    const char *head = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 10000\r\n\r\n";
    size_t hlen = strlen(head);
    char payload[10000];
    memset(payload, 'X', sizeof(payload));

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        send_all(sv[1], head, hlen);
        send_all(sv[1], payload, sizeof(payload));
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == 0);
    assert(header_len == hlen);
    assert(total_len >= hlen && total_len <= REQ_HEADER_MAX_BYTES);
    close(sv[0]);
    waitpid(pid, NULL, 0);
}

static void test_invalid_separators(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *bad = "GET / HTTP/1.1\r\nHost: a\n\n\r\r\n\n";
    assert(send(sv[1], bad, strlen(bad), 0) == (ssize_t)strlen(bad));
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == -1);
    close(sv[0]);
}

static void test_zero_eof(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == -1);
    close(sv[0]);
}

static void test_short_under_4_eof(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send(sv[1], "GE", 2, 0) == 2);
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == -1);
    close(sv[0]);
}

static void test_body_contains_crlfcrlf(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    const char *head = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 14\r\n\r\n";
    const char *req = "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 14\r\n\r\nhello\r\n\r\nworld";
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(send(sv[1], req, strlen(req), 0) == (ssize_t)strlen(req));
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == 0);
    assert(header_len == strlen(head));
    assert(total_len == strlen(req));
    assert(memcmp(buf + header_len, "hello\r\n\r\nworld", 14) == 0);
    close(sv[0]);
    close(sv[1]);
}

static void test_partial_eof(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const char *partial = "GET / HTTP/1.1\r\n";
    assert(send(sv[1], partial, strlen(partial), 0) == (ssize_t)strlen(partial));
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == -1);
    close(sv[0]);
}

static void test_exact_limit(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    char exact_buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    memset(exact_buf, 'A', sizeof(exact_buf) - 4);
    memcpy(exact_buf + sizeof(exact_buf) - 4, "\r\n\r\n", 4);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        send_all(sv[1], exact_buf, sizeof(exact_buf));
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == 0);
    assert(header_len == REQ_HEADER_MAX_BYTES);
    assert(total_len == REQ_HEADER_MAX_BYTES);
    close(sv[0]);
    waitpid(pid, NULL, 0);
}

static void test_over_limit(void) {
    int sv[2];
    char buf[REQ_HEADER_MAX_BYTES];
    char over_buf[REQ_HEADER_MAX_BYTES + 16];
    size_t header_len = 0;
    size_t total_len = 0;

    memset(over_buf, 'B', sizeof(over_buf));

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        send_all(sv[1], over_buf, sizeof(over_buf));
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    assert(recv_until_header_end(sv[0], buf, sizeof(buf), &header_len, &total_len) == -1);
    close(sv[0]);
    waitpid(pid, NULL, 0);
}

int main(void) {
    test_single_recv();
    test_split_boundary();
    test_with_trailing_body();
    test_header_plus_10kb_body();
    test_invalid_separators();
    test_zero_eof();
    test_short_under_4_eof();
    test_body_contains_crlfcrlf();
    test_partial_eof();
    test_exact_limit();
    test_over_limit();
    printf("[PASS] test_recv_header\n");
    return 0;
}
