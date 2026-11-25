#define _POSIX_C_SOURCE 200809L
#include "proxy_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

/* Non-blocking connect is bounded locally; this is not an event loop. */
int connect_backend(struct proxy_stream *stream, const struct sockaddr_in *backend) {
    struct timespec deadline;
    if (make_deadline(&deadline, PROXY_CONNECT_SECONDS) < 0) return -1;
    stream->backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (stream->backend_fd < 0) return -1;
    int fd = stream->backend_fd;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || set_close_on_exec(fd) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    if (connect(fd, (const struct sockaddr *)backend, sizeof(*backend)) < 0) {
        if (errno != EINPROGRESS && errno != EINTR) return -1;
        for (;;) {
            if (stream->running && !*stream->running) { errno = ECANCELED; return -1; }
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
            long long milliseconds = (deadline.tv_sec - now.tv_sec) * 1000LL +
                (deadline.tv_nsec - now.tv_nsec + 999999LL) / 1000000LL;
            if (milliseconds <= 0) { errno = ETIMEDOUT; return -1; }
            struct pollfd ready = {.fd = fd, .events = POLLOUT};
            int result = poll(&ready, 1, (int)milliseconds);
            if (result < 0 && errno == EINTR) continue;
            if (result < 0) return -1;
            if (result == 0) { errno = ETIMEDOUT; return -1; }
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return -1;
            if (error) { errno = error; return -1; }
            break;
        }
    }
    if (deadline_reached(&deadline)) { errno = ETIMEDOUT; return -1; }
    return fcntl(fd, F_SETFL, flags);
}

int make_deadline(struct timespec *deadline, int seconds) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) return -1;
    deadline->tv_sec += seconds;
    return 0;
}

bool deadline_reached(const struct timespec *deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return true;
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

int fill_backend_buffer(struct proxy_stream *stream) {
    if (stream->buffer_offset < stream->buffer_length) return 1;
    struct timespec deadline = stream->header_deadline;
    if (stream->body_started) {
        if (make_deadline(&deadline, PROXY_BODY_IDLE_SECONDS) < 0) return -1;
        if (stream->body_deadline.tv_sec < deadline.tv_sec ||
            (stream->body_deadline.tv_sec == deadline.tv_sec &&
             stream->body_deadline.tv_nsec < deadline.tv_nsec))
            deadline = stream->body_deadline;
    }
    struct io_context input = {stream->running, &deadline};
    ssize_t count = receive_with_deadline(stream->backend_fd, stream->buffer,
                                          sizeof(stream->buffer), &input);
    if (count < 0) {
        if (socket_io_timed_out()) errno = ETIMEDOUT;
        return -1;
    }
    stream->buffer_offset = 0;
    stream->buffer_length = (size_t)count;
    return count != 0;
}

int read_backend_byte(struct proxy_stream *stream, unsigned char *byte) {
    int available = fill_backend_buffer(stream);
    if (available < 0) return -1;
    if (available == 0) { errno = EPROTO; return -1; }
    *byte = stream->buffer[stream->buffer_offset++];
    return 0;
}

int read_backend_line(struct proxy_stream *stream, unsigned char *line,
                              size_t capacity, size_t *length) {
    *length = 0;
    while (*length < capacity) {
        unsigned char byte;
        if (read_backend_byte(stream, &byte) < 0) return -1;
        line[(*length)++] = byte;
        if (*length >= 2 && line[*length - 2] == '\r' && byte == '\n') {
            *length -= 2;
            return 0;
        }
    }
    errno = EPROTO;
    return -1;
}
