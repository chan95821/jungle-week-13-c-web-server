#define _POSIX_C_SOURCE 200809L
#include "../../src/proxy.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* 실제 루프백 리스너에 연결하고 호출자 FD의 수명·상속 방지·blocking 복귀를 확인한다. */
static void connects_to_local_listener(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    assert(listen(listener, 1) == 0);

    struct proxy_backend backend = {"127.0.0.1", ntohs(address.sin_port)};
    int fd = -1;
    assert(connect_backend(&backend, &fd) == 0);
    int fd_flags = fcntl(fd, F_GETFD), status_flags = fcntl(fd, F_GETFL);
    assert(fd >= 0 && fd_flags >= 0 && (fd_flags & FD_CLOEXEC) != 0);
    assert(status_flags >= 0 && (status_flags & O_NONBLOCK) == 0);
    int peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    assert(close(fd) == 0);
    char byte;
    assert(recv(peer, &byte, 1, 0) == 0);
    assert(close(peer) == 0 && close(listener) == 0);
}

/* IPv6가 활성화된 환경에서는 ::1의 실제 연결도 확인하며 미지원 환경은 명시적으로 건너뛴다. */
static void connects_to_ipv6_listener_when_available(void) {
    int listener = socket(AF_INET6, SOCK_STREAM, 0);
    if (listener < 0) {
        assert(errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT);
        puts("[SKIP] IPv6 sockets unavailable");
        return;
    }
    struct sockaddr_in6 address = {.sin6_family = AF_INET6};
    assert(inet_pton(AF_INET6, "::1", &address.sin6_addr) == 1);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0) {
        assert(errno == EADDRNOTAVAIL);
        assert(close(listener) == 0);
        puts("[SKIP] IPv6 loopback unavailable");
        return;
    }
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    assert(listen(listener, 1) == 0);
    struct proxy_backend backend = {"::1", ntohs(address.sin6_port)};
    int fd;
    assert(connect_backend(&backend, &fd) == 0);
    int peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    assert(close(fd) == 0 && close(peer) == 0 && close(listener) == 0);
}

/* bind만 하고 listen하지 않은 포트를 유지해 경합 없이 연결 거절을 재현한다. */
static void rejects_closed_service(void) {
    int reserved = socket(AF_INET, SOCK_STREAM, 0);
    assert(reserved >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET};
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(bind(reserved, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(reserved, (struct sockaddr *)&address, &length) == 0);

    struct proxy_backend backend = {"127.0.0.1", ntohs(address.sin_port)};
    for (int i = 0; i < 10; ++i) {
        int fd = 123;
        assert(connect_backend(&backend, &fd) == 502 && fd == -1);
    }
    assert(close(reserved) == 0);
}

/* DNS 조회를 시작하지 않고 잘못된 정적 설정을 내부 오류로 거부한다. */
static void rejects_invalid_addresses(void) {
    const struct proxy_backend cases[] = {
        {"localhost", 8001}, {"not-an-ip", 8001}, {NULL, 8001}, {"127.0.0.1", 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int fd = 123;
        assert(connect_backend(&cases[i], &fd) == 500 && fd == -1);
    }
}

/* 실제 TCP 연결 계약을 확인한다. 네트워크 블랙홀 타임아웃 재현은 별도 범위다. */
int main(void) {
    rejects_invalid_addresses();
    connects_to_local_listener();
    connects_to_ipv6_listener_when_available();
    rejects_closed_service();
    puts("[PASS] test_proxy_connect");
    return 0;
}
