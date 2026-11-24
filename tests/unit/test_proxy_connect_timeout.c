#define _POSIX_C_SOURCE 200809L
#include "../../src/proxy.h"
#include "../../src/io_utils.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int attempted_fd;
static int wait_calls;

/* 실제로 만든 소켓에서 연결 진행 중이라는 결과만 대역 처리한다. */
static int simulate_connect_in_progress(int fd, const struct sockaddr *address, socklen_t length) {
    (void)address;
    (void)length;
    attempted_fd = fd;
    errno = EINPROGRESS;
    return -1;
}

/* 외부 네트워크 상태에 의존하지 않고 쓰기 대기의 타임아웃을 재현한다. */
static int simulate_connection_timeout(int fd, short events, const struct timespec *deadline) {
    assert(fd == attempted_fd && events == POLLOUT && deadline != NULL);
    ++wait_calls;
    errno = ETIMEDOUT;
    return -1;
}

/* 내부 함수를 노출하지 않고 OS 연결·대기 결과만 교체하는 기존 white-box 테스트 방식이다. */
#define connect simulate_connect_in_progress
#define wait_fd_ready simulate_connection_timeout
#include "../../src/proxy_connect.c"
#undef connect
#undef wait_fd_ready

/* 대기 초과 시 504, FD -1, 실제 시도 소켓의 닫힘을 함께 확인한다. */
int main(void) {
    struct proxy_backend backend = {"127.0.0.1", 8001};
    int fd = 123;
    assert(connect_backend(&backend, &fd) == 504);
    assert(fd == -1 && wait_calls == 1);
    assert(fcntl(attempted_fd, F_GETFD) == -1 && errno == EBADF);
    puts("[PASS] test_proxy_connect_timeout");
    return 0;
}
