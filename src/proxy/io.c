#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

bool is_server_running(const volatile sig_atomic_t *running) {
    return !running || *running;
}

static int64_t get_monotonic_time_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool is_retryable_socket_error(void) {
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

/* EINTR는 진행이 아니므로 원래 deadline을 유지한다.
 * poll은 한 FD의 준비 상태만 기다린다. 다중 연결 이벤트 루프는 아니다. */
static int wait_until_socket_ready(int fd, short events, int64_t deadline,
                            const volatile sig_atomic_t *running) {
    for (;;) {
        if (!is_server_running(running)) {
            errno = ECANCELED;
            return -1;
        }

        int64_t now = get_monotonic_time_ms();
        if (now < 0) return -1;
        int64_t remaining_ms = deadline - now;
        if (remaining_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct pollfd socket_event = {.fd = fd, .events = events};
        int timeout_ms = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
        int ready = poll(&socket_event, 1, timeout_ms);

        if (!is_server_running(running)) {
            errno = ECANCELED;
            return -1;
        }
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (socket_event.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }

        /* HUP/ERR도 recv/send/SO_ERROR로 확인한다. HUP일 때에도 커널에
         * 마지막 데이터가 남아 있을 수 있으므로 즉시 EOF로 간주하지 않는다. */
        return 0;
    }
}

int connect_to_backend(const struct sockaddr_in *address,
                          const volatile sig_atomic_t *running) {
    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) return -1;

    int flags = fcntl(backend_fd, F_GETFL);
    if (flags < 0 ||
        fcntl(backend_fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(backend_fd, F_SETFD, FD_CLOEXEC) < 0) goto failed;

    int64_t started_at = get_monotonic_time_ms();
    if (started_at < 0) goto failed;

    if (connect(backend_fd, (const struct sockaddr *)address, sizeof(*address)) < 0) {
        if (errno != EINPROGRESS && errno != EINTR) goto failed;
        if (wait_until_socket_ready(backend_fd, POLLOUT,
                            started_at + PROXY_BACKEND_TIMEOUT_MS, running) < 0) goto failed;

        /* 쓰기 준비가 연결 성공을 뜻하지는 않는다. 연결 거절도 준비 상태로
         * 보일 수 있어 POSIX getsockopt의 SO_ERROR로 결과를 확인한다. */
        int connect_error = 0;
        socklen_t error_length = sizeof(connect_error);
        if (getsockopt(backend_fd, SOL_SOCKET, SO_ERROR,
                       &connect_error, &error_length) < 0) goto failed;
        if (connect_error != 0) {
            errno = connect_error;
            goto failed;
        }
    }
    return backend_fd;

failed: {
        /* 성공 시에만 호출자에게 FD 소유권을 넘긴다. 실패 FD는 여기서 정리. */
        int saved_errno = errno;
        close(backend_fd);
        errno = saved_errno;
        return -1;
    }
}

/* CS:APP RIO의 완전 송신과 같은 원리로 short count를 누적하는 직접 구현.
 * 실제 송신이 진행했을 때만 무진행 기한을 갱신한다. 전체 응답 기한은 아니다. */
int send_all_bytes(int fd, const void *data, size_t length, int timeout_ms,
                    const volatile sig_atomic_t *running) {
    const unsigned char *next_byte = data;
    int64_t last_progress = get_monotonic_time_ms();
    if (last_progress < 0) return -1;

    while (length > 0) {
        if (wait_until_socket_ready(fd, POLLOUT, last_progress + timeout_ms, running) < 0) return -1;

        ssize_t sent = send(fd, next_byte, length, 0);
        if (sent < 0 && is_retryable_socket_error()) continue;
        if (sent < 0) return -1;
        if (sent == 0) {
            errno = EIO;
            return -1;
        }

        next_byte += sent;
        length -= (size_t)sent;
        last_progress = get_monotonic_time_ms();
        if (last_progress < 0) return -1;
    }
    return 0;
}

ssize_t receive_backend_bytes(struct proxy_reader *reader) {
    int64_t started_at = get_monotonic_time_ms();
    if (started_at < 0) return -1;

    for (;;) {
        if (wait_until_socket_ready(reader->backend_fd, POLLIN,
                            started_at + PROXY_BACKEND_TIMEOUT_MS, reader->running) < 0) return -1;

        ssize_t received = recv(reader->backend_fd, reader->buffer, sizeof(reader->buffer), 0);
        if (received < 0 && is_retryable_socket_error()) continue;
        if (received >= 0) {
            reader->position = 0;
            reader->length = (size_t)received;
        }
        return received;
    }
}

int read_backend_byte(struct proxy_reader *reader, unsigned char *byte) {
    if (reader->position == reader->length) {
        ssize_t received = receive_backend_bytes(reader);
        if (received < 0) return -1;
        if (received == 0) {
            errno = EPROTO;
            return -1;
        }
    }

    /* 메모리에서 한 바이트 소비한다. 매 바이트마다 recv하는 구조가 아니다. */
    *byte = reader->buffer[reader->position++];
    return 0;
}

int read_backend_line(struct proxy_reader *reader, unsigned char *line,
                           size_t capacity, size_t *length) {
    *length = 0;
    while (*length < capacity) {
        if (read_backend_byte(reader, line + *length) < 0) return -1;
        ++*length;

        if (*length >= 2 && line[*length - 2] == '\r' && line[*length - 1] == '\n') return 0;
    }
    errno = EPROTO;
    return -1;
}
