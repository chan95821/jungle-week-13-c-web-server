#define _POSIX_C_SOURCE 200809L
#include "socket_io.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Check whether server shutdown has not been requested. */
bool server_is_running(const struct io_context *io) {
    return !io || !io->running || *io->running != 0;
}

/* Return true when monotonic clock has reached or passed deadline. */
static bool deadline_passed(const struct timespec *now, const struct timespec *deadline) {
    return now->tv_sec > deadline->tv_sec ||
           (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

/* A positive, rounded-up timeout: zero would disable SO_*TIMEO entirely. */
static int set_call_timeout(int fd, int option, const struct io_context *io) {
    if (!io) return 0;
    if (!server_is_running(io)) { errno = ECANCELED; return -1; }

    struct timeval timeout = {.tv_sec = IO_CALL_TIMEOUT_SECONDS, .tv_usec = 0};
    if (io->deadline) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        if (deadline_passed(&now, io->deadline)) { errno = ETIMEDOUT; return -1; }

        time_t seconds = io->deadline->tv_sec - now.tv_sec;
        long nanoseconds = io->deadline->tv_nsec - now.tv_nsec;
        if (nanoseconds < 0) { --seconds; nanoseconds += 1000000000L; }

        if (seconds < IO_CALL_TIMEOUT_SECONDS) {
            timeout.tv_sec = seconds;
            timeout.tv_usec = (nanoseconds + 999L) / 1000L;
            if (timeout.tv_usec == 1000000L) {
                ++timeout.tv_sec;
                timeout.tv_usec = 0;
            }
        }
    }

    return setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

/* Identify socket timeout errors across SO_RCVTIMEO/SO_SNDTIMEO implementations. */
bool socket_io_timed_out(void) {
    return errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK;
}

/* Receive up to len bytes while honoring io cancellation, EINTR, and deadline. */
ssize_t receive_with_deadline(int fd, void *buf, size_t len, const struct io_context *io) {
    for (;;) {
        if (set_call_timeout(fd, SO_RCVTIMEO, io) < 0) return -1;
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

/* Send all len bytes, retrying short writes and EINTR. Return 0 or -1. */
int send_all_with_deadline(int fd, const void *buf, size_t len, const struct io_context *io) {
    if (len == 0) return 0;
    if (!buf) { errno = EINVAL; return -1; }

    const unsigned char *data = buf;
    size_t sent = 0;
    while (sent < len) {
        if (set_call_timeout(fd, SO_SNDTIMEO, io) < 0) return -1;

        ssize_t count = send(fd, data + sent, len - sent, 0);
        if (!server_is_running(io)) { errno = ECANCELED; return -1; }
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) { errno = EIO; return -1; }
        sent += (size_t)count;
    }

    return 0;
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

