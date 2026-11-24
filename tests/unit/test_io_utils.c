#define _POSIX_C_SOURCE 200809L
#include "../../src/io_utils.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 같은 읽기에 들어온 헤더·본문을 전체 길이와 헤더 끝 위치로 구분해 보존한다. */
static void preserves_prefetched_body(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const unsigned char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na\0b";
    assert(write(pair[1], wire, sizeof(wire) - 1) == (ssize_t)(sizeof(wire) - 1));
    assert(shutdown(pair[1], SHUT_WR) == 0);
    unsigned char buffer[128];
    size_t received, header_end;
    assert(receive_http_headers(pair[0], buffer, sizeof(buffer), &received, &header_end, NULL) == 0);
    assert(header_end == sizeof(wire) - 1 - 3);
    assert(received >= header_end && received <= sizeof(wire) - 1);
    assert(memcmp(buffer, wire, received) == 0);
    while (received < sizeof(wire) - 1) {
        ssize_t count = recv(pair[0], buffer + received, sizeof(buffer) - received, 0);
        assert(count > 0);
        received += (size_t)count;
    }
    assert(memcmp(buffer + header_end, "a\0b", 3) == 0);
    close(pair[0]);
    close(pair[1]);
}

/* 실제 FD로 양방향 준비 대기와 지난 마감 시각의 타임아웃을 확인한다. */
int main(void) {
    preserves_prefetched_body();
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(set_fd_close_on_exec(pair[0]) == 0);
    assert((fcntl(pair[0], F_GETFD) & FD_CLOEXEC) != 0);
    struct timespec deadline;
    assert(make_io_deadline(&deadline, 1000));
    assert(wait_fd_ready(pair[0], POLLOUT, &deadline) == 0);
    assert(write(pair[1], "x", 1) == 1);
    assert(wait_fd_ready(pair[0], POLLIN, &deadline) == 0);

    deadline = (struct timespec){0};
    assert(wait_fd_ready(pair[0], POLLIN, &deadline) == -1 && errno == ETIMEDOUT);
    const struct io_context expired = {.deadline = &deadline};
    assert(send_all_bytes(pair[0], "x", 1, &expired) == -1 && errno == ETIMEDOUT);
    volatile sig_atomic_t running = 0;
    const struct io_context cancelled = {.running = &running};
    assert(send_all_bytes(pair[0], "x", 1, &cancelled) == -1 && errno == ECANCELED);
    assert(set_fd_close_on_exec(-1) == -1 && errno == EBADF);
    assert(close(pair[0]) == 0 && close(pair[1]) == 0);
    puts("[PASS] test_io_utils");
    return 0;
}
