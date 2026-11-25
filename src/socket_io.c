#define _POSIX_C_SOURCE 200809L
#include "socket_io.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Check whether cancellation has not been requested. */
bool cancel_signal_is_running(const struct cancel_signal *cancel) {
    return !cancel || !cancel->running || *cancel->running != 0;
}

/* Check whether server shutdown has not been requested. */
bool server_is_running(const struct io_context *io) {
    return !io || cancel_signal_is_running(io->cancel);
}

/* Return true when monotonic clock has reached or passed deadline. */
static bool deadline_passed(const struct timespec *now, const struct timespec *deadline) {
    return now->tv_sec > deadline->tv_sec ||
           (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

/* Compute clamped remaining time until deadline, or -1 (ETIMEDOUT) if passed. */
static int compute_remaining_time(const struct timespec *deadline,
                                  time_t *out_seconds, long *out_nanoseconds) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    if (deadline_passed(&now, deadline)) { errno = ETIMEDOUT; return -1; }

    time_t seconds = deadline->tv_sec - now.tv_sec;
    long nanoseconds = deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds < 0) { --seconds; nanoseconds += 1000000000L; }

    if (seconds >= IO_CALL_TIMEOUT_SECONDS) {
        seconds = IO_CALL_TIMEOUT_SECONDS;
        nanoseconds = 0;
    }
    *out_seconds = seconds;
    *out_nanoseconds = nanoseconds;
    return 0;
}

/* Apply 5s kernel socket I/O timeouts once per socket as a blocking safety net. */
int socket_configure_timeouts(int fd) {
    const struct timeval timeout = {.tv_sec = IO_CALL_TIMEOUT_SECONDS, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    return 0;
}

/* Wait until fd is ready for events, respecting io deadline and optional stop_fd. */
static int wait_socket_ready(int fd, short events, const struct io_context *io) {
    if (!io) return 0;

    int stop_fd = io->cancel ? io->cancel->stop_fd : -1;
    nfds_t nfds = stop_fd >= 0 ? 2 : 1;

    for (;;) {
        if (!server_is_running(io)) { errno = ECANCELED; return -1; }

        time_t seconds = IO_CALL_TIMEOUT_SECONDS;
        long nanoseconds = 0;
        if (io->deadline && compute_remaining_time(io->deadline, &seconds, &nanoseconds) < 0) {
            return -1;
        }

        int timeout_ms = (int)(seconds * 1000 + (nanoseconds + 999999L) / 1000000L);
        if (timeout_ms <= 0) timeout_ms = 1;

        struct pollfd fds[2] = {
            {.fd = fd, .events = events},
            {.fd = stop_fd, .events = POLLIN}
        };
        int ready = poll(fds, nfds, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if ((nfds == 2 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) ||
            !server_is_running(io)) {
            errno = ECANCELED;
            return -1;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        return 0;
    }
}

/* Identify socket timeout errors across SO_RCVTIMEO/SO_SNDTIMEO implementations. */
bool socket_io_timed_out(void) {
    return errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK;
}

/* Receive up to len bytes while honoring io cancellation, EINTR, and deadline. */
ssize_t receive_with_deadline(int fd, void *buf, size_t len, const struct io_context *io) {
    for (;;) {
        if (wait_socket_ready(fd, POLLIN, io) < 0) return -1;
        ssize_t count = recv(fd, buf, len, 0);
        if (!server_is_running(io)) { errno = ECANCELED; return -1; }
        if (count < 0 && errno == EINTR) continue;
        if (count > 0 && io && io->deadline) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
            if (deadline_passed(&now, io->deadline)) { errno = ETIMEDOUT; return -1; }
        }
        return count;
    }
}

/* Advance vector cursor past written bytes across pending[head..count-1]. */
static void advance_iov_cursor(struct iovec *pending, size_t *head,
                               size_t count, size_t written) {
    while (*head < count && written >= pending[*head].iov_len) {
        written -= pending[*head].iov_len;
        (*head)++;
    }
    if (*head < count && written > 0) {
        pending[*head].iov_base = (unsigned char *)pending[*head].iov_base + written;
        pending[*head].iov_len -= written;
    }
}

/* Write all non-empty scatter/gather vectors, retrying short writes and EINTR. */
int send_allv_with_deadline(int fd, const struct iovec *iov, int iovcnt,
                            const struct io_context *io) {
    enum { SEND_IOV_STACK_MAX = 16 };

    if (!iov || iovcnt <= 0 || iovcnt > SEND_IOV_STACK_MAX) {
        errno = EINVAL;
        return -1;
    }

    struct iovec pending[SEND_IOV_STACK_MAX];
    size_t pending_count = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len == 0) continue;
        if (!iov[i].iov_base) {
            errno = EINVAL;
            return -1;
        }
        pending[pending_count++] = iov[i];
    }
    if (pending_count == 0) return 0;

    size_t head = 0;
    while (head < pending_count) {
        if (wait_socket_ready(fd, POLLOUT, io) < 0) return -1;

        ssize_t count = writev(fd, &pending[head], (int)(pending_count - head));
        if (!server_is_running(io)) { errno = ECANCELED; return -1; }
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) { errno = EIO; return -1; }

        advance_iov_cursor(pending, &head, pending_count, (size_t)count);
    }

    return 0;
}

/* Send all len bytes, retrying short writes and EINTR. Return 0 or -1. */
int send_all_with_deadline(int fd, const void *buf, size_t len, const struct io_context *io) {
    if (len == 0) return 0;
    if (!buf) { errno = EINVAL; return -1; }

    struct iovec single = {.iov_base = (void *)buf, .iov_len = len};
    return send_allv_with_deadline(fd, &single, 1, io);
}

int send_all(int fd, const void *buf, size_t len) {
    return send_all_with_deadline(fd, buf, len, NULL);
}

int parse_port(const char *arg, uint16_t *out_port) {
    if (!arg || !out_port || !isdigit((unsigned char)arg[0])) return -1;
    errno = 0;
    char *endptr;
    long value = strtol(arg, &endptr, 10);
    if (errno == ERANGE || endptr == arg || *endptr != '\0' || value < 1 || value > 65535) return -1;
    *out_port = (uint16_t)value;
    return 0;
}

/* Mark fd close-on-exec so socket FDs are never inherited by CGI children. */
int set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int open_listenfd(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    if (set_close_on_exec(fd) < 0) {
        perror("listener cloexec");
        close(fd);
        return -1;
    }

    const int enabled = 1;
    const struct timeval timeout = {.tv_sec = IO_CALL_TIMEOUT_SECONDS, .tv_usec = 0};
    /* Bound accept as well, so a pre-call shutdown signal cannot strand it. */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("listener options");
        close(fd);
        return -1;
    }

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)}
    };
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen"); close(fd); return -1;
    }

    return fd;
}

