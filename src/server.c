#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "router.h"
#include "static.h"
#include "greet.h"
#include <inttypes.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct io_context {
    const volatile sig_atomic_t *running;
    const struct timespec *deadline;
};

static bool active(const struct io_context *io) {
    return !io || !io->running || *io->running != 0;
}

static bool deadline_passed(const struct timespec *now, const struct timespec *deadline) {
    return now->tv_sec > deadline->tv_sec ||
           (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

/* A positive, rounded-up timeout: zero would disable SO_*TIMEO entirely. */
static int set_call_timeout(int fd, int option, const struct io_context *io) {
    if (!io) return 0;
    if (!active(io)) { errno = ECANCELED; return -1; }
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

static bool timed_out(void) {
    return errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK;
}

static ssize_t receive_some(int fd, void *buf, size_t len, const struct io_context *io) {
    for (;;) {
        if (set_call_timeout(fd, SO_RCVTIMEO, io) < 0) return -1;
        ssize_t count = recv(fd, buf, len, 0);
        if (!active(io)) { errno = ECANCELED; return -1; }
        if (count < 0 && errno == EINTR) continue;
        if (count > 0 && io && io->deadline) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
            if (deadline_passed(&now, io->deadline)) { errno = ETIMEDOUT; return -1; }
        }
        return count;
    }
}

static int send_bytes(int fd, const void *buf, size_t len, const struct io_context *io) {
    if (len == 0) return 0;
    if (!buf) { errno = EINVAL; return -1; }
    const unsigned char *data = buf;
    size_t sent = 0;
    while (sent < len) {
        if (set_call_timeout(fd, SO_SNDTIMEO, io) < 0) return -1;
        ssize_t count = send(fd, data + sent, len - sent, 0);
        if (!active(io)) { errno = ECANCELED; return -1; }
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) { errno = EIO; return -1; }
        sent += (size_t)count;
    }
    return 0;
}

/* 0: complete, 1: EOF before completion, -1: errno describes the error. */
static int receive_header(int fd, unsigned char *buf, size_t cap,
                          size_t *received, size_t *header_end,
                          const struct io_context *io) {
    *received = 0;
    *header_end = 0;
    if (!buf || cap < 4) { errno = EINVAL; return -1; }
    while (*received < cap) {
        size_t previous = *received;
        ssize_t count = receive_some(fd, buf + previous, cap - previous, io);
        if (count < 0) return -1;
        if (count == 0) return 1;
        *received += (size_t)count;
        size_t start = previous > 3 ? previous - 3 : 0;
        for (size_t i = start; i + 4 <= *received; ++i) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                *header_end = i + 4;
                return 0;
            }
        }
    }
    errno = EMSGSIZE;
    return -1;
}

int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len) {
    size_t received, header_end;
    if (receive_header(fd, (unsigned char *)buf, cap, &received, &header_end, NULL) != 0) return -1;
    if (out_len) *out_len = received;
    return 0;
}

int send_all(int fd, const void *buf, size_t len) {
    return send_bytes(fd, buf, len, NULL);
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

int open_listenfd(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
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

static int send_file_body(int connfd, int filefd, uintmax_t remaining,
                          const struct io_context *io) {
    unsigned char buffer[FILE_SEND_BUFFER_BYTES];
    while (remaining) {
        if (!active(io)) { errno = ECANCELED; return -1; }
        size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t count = read(filefd, buffer, wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) { errno = EIO; return -1; } /* Truncated after fstat. */
        if (send_bytes(connfd, buffer, (size_t)count, io) < 0) return -1;
        remaining -= (uintmax_t)count;
    }
    return 0;
}

static int send_response(int fd, const struct router_response *response,
                         bool head_request, const struct io_context *io) {
    char header[512];
    char allow_line[32] = "";
    if (response->allow) {
        int count = snprintf(allow_line, sizeof(allow_line), "Allow: %s\r\n", response->allow);
        if (count < 0 || (size_t)count >= sizeof(allow_line)) { errno = EOVERFLOW; return -1; }
    }
    int count;
    if (head_request) {
        /* HEAD remains unsupported (501) and must not carry content. */
        count = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%sConnection: close\r\n\r\n",
                         response->status, http_reason(response->status),
                         response->content_type, allow_line);
    } else {
        count = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %" PRIuMAX "\r\n"
                         "%sConnection: close\r\n\r\n",
                         response->status, http_reason(response->status),
                         response->content_type, response->length, allow_line);
    }
    if (count < 0 || (size_t)count >= sizeof(header)) { errno = EOVERFLOW; return -1; }
    if (send_bytes(fd, header, (size_t)count, io) < 0) return -1;
    if (head_request) return 0;
    if (response->kind == ROUTER_BODY_FILE)
        return send_file_body(fd, response->file_fd, response->length, io);
    if (response->length > SIZE_MAX) { errno = EOVERFLOW; return -1; }
    return send_bytes(fd, response->body, (size_t)response->length, io);
}
static void finish_response(int fd, const struct io_context *io) {
    if (!active(io) || shutdown(fd, SHUT_WR) < 0) return;
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) return;
    deadline.tv_sec += DRAIN_DEADLINE_SECONDS;
    struct io_context drain = {io->running, &deadline};
    unsigned char discard[1024];
    size_t discarded = 0;
    while (active(&drain) && discarded < DRAIN_MAX_BYTES) {
        size_t room = DRAIN_MAX_BYTES - discarded;
        if (room > sizeof(discard)) room = sizeof(discard);
        ssize_t count = receive_some(fd, discard, room, &drain);
        if (count <= 0) break;
        discarded += (size_t)count;
    }
}

void handle_client(int connfd, const volatile sig_atomic_t *running,
                   const struct timespec *accepted_at, int rootfd) {
    unsigned char header[REQ_HEADER_MAX_BYTES];
    unsigned char *body = NULL;
    int filefd = -1;
    unsigned char *generated_body = NULL;
    struct http_request request = {0};
    enum http_route route = HTTP_ROUTE_HELLO;
    const char *allow = NULL;
    size_t received = 0, header_end = 0, body_received = 0;
    bool head_request = false;
    struct timespec header_deadline = *accepted_at;
    header_deadline.tv_sec += HEADER_DEADLINE_SECONDS;
    struct io_context input = {running, &header_deadline};
    const struct io_context output = {running, NULL};
    int status = 0;

    int result = receive_header(connfd, header, sizeof(header),
                                &received, &header_end, &input);
    if (!active(&input)) goto cleanup;
    if (result == 1) {
        if (received == 0) goto cleanup;
        status = 400;
        goto respond;
    }
    if (result < 0) {
        if (errno == EMSGSIZE) status = 431;
        else if (timed_out()) status = 408;
        else { perror("receive header"); goto cleanup; }
        goto respond;
    }
    struct timespec body_deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &body_deadline) < 0) {
        status = 500;
        goto respond;
    }
    body_deadline.tv_sec += BODY_DEADLINE_SECONDS;
    status = http_parse_request(header, header_end, &request);
    head_request = http_slice_equal(request.method, "HEAD");
    if (status) goto respond;
    status = http_route_request(&request, &route, &allow);
    if (status) goto respond;

    if (request.content_length) {
        body = malloc(request.content_length);
        if (!body) { status = 500; goto respond; }
        body_received = received - header_end;
        if (body_received > request.content_length) body_received = request.content_length;
        memcpy(body, header + header_end, body_received);
    }
    if (request.expect_continue && body_received < request.content_length) {
        static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send_bytes(connfd, interim, sizeof(interim) - 1, &output) < 0) {
            if (active(&output)) perror("send 100 Continue");
            goto cleanup;
        }
    }
    input.deadline = &body_deadline;
    while (body_received < request.content_length) {
        ssize_t count = receive_some(connfd, body + body_received,
                                     request.content_length - body_received, &input);
        if (!active(&input)) goto cleanup;
        if (count == 0) { status = 400; goto respond; }
        if (count < 0) {
            if (timed_out()) { status = 408; goto respond; }
            perror("receive body");
            goto cleanup;
        }
        body_received += (size_t)count;
    }
    request.body = (struct http_slice){body, body_received};
    status = 200;

respond:
    if (active(&output)) {
        struct router_response response =
            router_prepare_response(&request, route, status, allow);
        if (status == 200 && route == HTTP_ROUTE_GREET)
            response = greet_prepare_response(&request, &generated_body);
        if (status == 200 && route == HTTP_ROUTE_STATIC) {
            int file_status = static_open_file(rootfd, request.path, &filefd,
                                               &response.length, &response.content_type);
            if (file_status) {
                response = router_prepare_response(&request, route, file_status, NULL);
            } else {
                response.kind = ROUTER_BODY_FILE;
                response.body = NULL;
                response.file_fd = filefd;
            }
        }
        if (send_response(connfd, &response, head_request, &output) == 0) {
            finish_response(connfd, &output);
        } else if (active(&output)) {
            perror("send response");
        }
    }
cleanup:
    /* Sole owner of body storage; input/output helpers never release it. */
    if (filefd >= 0 && close(filefd) < 0) perror("close file");
    free(generated_body);
    free(body);
}
