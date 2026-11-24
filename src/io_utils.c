#define _POSIX_C_SOURCE 200809L
#include "io_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string.h>

/* 기존 receive_some()의 제한·취소 검사를 요청/백엔드 수신에 동일하게 적용한다. */
ssize_t receive_socket_bytes(int fd, void *buffer, size_t capacity, const struct io_context *io) {
    for (;;) {
        if (io_set_call_timeout(fd, SO_RCVTIMEO, io) < 0) return -1;
        ssize_t count = recv(fd, buffer, capacity, 0);
        if (!io_is_active(io)) { errno = ECANCELED; return -1; }
        if (count < 0 && errno == EINTR) continue;
        if (count > 0 && io && io->deadline) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
            if (io_deadline_passed(&now, io->deadline)) { errno = ETIMEDOUT; return -1; }
        }
        return count;
    }
}

/* 기존 요청 헤더 수신의 경계 탐색을 공유한다. 직전 읽기의 마지막 3바이트부터 다시 살펴
 * 분할된 CRLFCRLF도 찾는다. 헤더 뒤에 함께 읽은 본문은 버리지 않는다. */
int receive_http_headers(int fd, unsigned char *buffer, size_t capacity,
                          size_t *received, size_t *header_end, const struct io_context *io) {
    *received = 0;
    *header_end = 0;
    if (!buffer || capacity < 4) { errno = EINVAL; return -1; }

    while (*received < capacity) {
        size_t previous = *received;
        ssize_t count = receive_socket_bytes(fd, buffer + previous, capacity - previous, io);
        if (count < 0) return -1;
        if (count == 0) return 1;
        *received += (size_t)count;

        size_t start = previous > 3 ? previous - 3 : 0;
        for (size_t i = start; i + 4 <= *received; ++i) {
            if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) {
                *header_end = i + 4;
                return 0;
            }
        }
    }
    errno = EMSGSIZE;
    return -1;
}

/* 기존 서버의 종료 플래그를 공유 송수신에서도 같은 의미로 해석한다. */
bool io_is_active(const struct io_context *io) {
    return !io || !io->running || *io->running != 0;
}

/* 단조 시각 기준 절대 마감을 비교하며 상태를 바꾸지 않는다. */
bool io_deadline_passed(const struct timespec *now, const struct timespec *deadline) {
    return now->tv_sec > deadline->tv_sec ||
           (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

/* 기존 set_call_timeout()을 공유한다. 0 timeval은 무제한이므로 양수로 올림한다.
 * 각 호출은 최대 5초이며 전체 마감이 더 이르면 남은 시간만 적용한다. */
int io_set_call_timeout(int fd, int option, const struct io_context *io) {
    if (!io) return 0;
    if (!io_is_active(io)) { errno = ECANCELED; return -1; }
    struct timeval timeout = {.tv_sec = IO_CALL_TIMEOUT_SECONDS};
    if (io->deadline) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        if (io_deadline_passed(&now, io->deadline)) { errno = ETIMEDOUT; return -1; }
        time_t seconds = io->deadline->tv_sec - now.tv_sec;
        long nanoseconds = io->deadline->tv_nsec - now.tv_nsec;
        if (nanoseconds < 0) { --seconds; nanoseconds += 1000000000L; }
        if (seconds < IO_CALL_TIMEOUT_SECONDS) {
            timeout.tv_sec = seconds;
            timeout.tv_usec = (nanoseconds + 999L) / 1000L;
            if (timeout.tv_usec == 1000000L) { ++timeout.tv_sec; timeout.tv_usec = 0; }
        }
    }
    return setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

/* 기존 send_bytes()의 부분 송신 루프를 서버와 프록시가 공유한다.
 * 마감이 있다면 마지막 송신 후에도 확인해 부분 진행으로 총시간 제한을 우회하지 못하게 한다. */
int send_all_bytes(int fd, const void *buffer, size_t length, const struct io_context *io) {
    if (length == 0) return 0;
    if (!buffer) { errno = EINVAL; return -1; }
    const unsigned char *data = buffer;
    size_t sent = 0;
    while (sent < length) {
        if (io_set_call_timeout(fd, SO_SNDTIMEO, io) < 0) return -1;
        ssize_t count = send(fd, data + sent, length - sent, 0);
        if (!io_is_active(io)) { errno = ECANCELED; return -1; }
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) { errno = EIO; return -1; }
        sent += (size_t)count;

        if (io && io->deadline) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
            if (io_deadline_passed(&now, io->deadline)) { errno = ETIMEDOUT; return -1; }
        }
    }
    return 0;
}

/* 서버·백엔드 소켓이 exec한 CGI 프로그램에 남지 않도록 기존 FD 플래그에 추가한다. */
int set_fd_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* CGI에서 쓰던 마감 계산을 공유한다. 음수 제한은 거부하며 시계 조회 실패를 전달한다. */
bool make_io_deadline(struct timespec *deadline, int timeout_ms) {
    if (timeout_ms < 0) { errno = EINVAL; return false; }
    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) return false;
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return true;
}

/* CGI의 읽기 대기를 연결 완료의 쓰기 대기에도 재사용한다. 마감은 재시도 때 연장하지 않는다.
 * POLLERR/POLLHUP은 호출자가 실제 I/O 결과를 조사하도록 준비 완료로 돌려준다. */
int wait_fd_ready(int fd, short events, const struct timespec *deadline) {
    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        int64_t seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
        long nanoseconds = deadline->tv_nsec - now.tv_nsec;
        if (nanoseconds < 0) { --seconds; nanoseconds += 1000000000L; }
        if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
            errno = ETIMEDOUT;
            return -1;
        }
        int timeout = seconds >= INT_MAX / 1000 ? INT_MAX :
                      (int)(seconds * 1000 + (nanoseconds + 999999L) / 1000000L);
        struct pollfd pfd = {.fd = fd, .events = events};
        int ready = poll(&pfd, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) { errno = ETIMEDOUT; return -1; }
        if (pfd.revents & POLLNVAL) { errno = EBADF; return -1; }
        return 0;
    }
}
