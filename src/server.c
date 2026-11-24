#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "router.h"
#include "io_utils.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Identify socket timeout errors across SO_RCVTIMEO/SO_SNDTIMEO implementations. */
static bool timed_out(void) {
    return errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK;
}

/* 기존 수신 진입점. 바이트 수신·취소·마감 처리는 공통 유틸을 사용한다. */
static ssize_t receive_some(int fd, void *buf, size_t len, const struct io_context *io) {
    return receive_socket_bytes(fd, buf, len, io);
}

/* 기존 서버 송신 진입점. 완전 송신과 I/O 제어는 공통 유틸을 사용한다. */
static int send_bytes(int fd, const void *buf, size_t len, const struct io_context *io) {
    return send_all_bytes(fd, buf, len, io);
}

/* 0: complete, 1: EOF before completion, -1: errno describes the error. */
static int receive_header(int fd, unsigned char *buf, size_t cap,
                          size_t *received, size_t *header_end,
                          const struct io_context *io) {
    return receive_http_headers(fd, buf, cap, received, header_end, io);
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
    if (set_fd_close_on_exec(fd) < 0) {
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

/* Stream exactly length bytes from provider; fail with EIO on short/oversized/null chunks. */
static int send_body(int fd, struct body_provider *provider, size_t length,
                     const struct io_context *io) {
    size_t remaining = length;
    while (remaining > 0) {
        struct http_slice chunk = {0};
        errno = 0;
        enum body_result result = provider->next_func(provider->state, &chunk);
        if (result != BODY_DATA) {
            if (result != BODY_ERROR || errno == 0) errno = EIO;
            return -1;
        }
        if (!chunk.data || chunk.length == 0 || chunk.length > remaining) {
            errno = EIO;
            return -1;
        }
        if (send_bytes(fd, chunk.data, chunk.length, io) < 0) return -1;
        remaining -= chunk.length;
    }
    return 0;
}

/* Stream HTTP/1.1 chunked frames; omit terminating 0-chunk on provider or send error. */
static int send_chunked_body(int fd, struct body_provider *provider,
                             const struct io_context *io) {
    for (;;) {
        struct http_slice chunk = {0};
        errno = 0;
        enum body_result result = provider->next_func(provider->state, &chunk);
        if (result == BODY_END) return send_bytes(fd, "0\r\n\r\n", 5, io);
        if (result != BODY_DATA) {
            if (result != BODY_ERROR || errno == 0) errno = EIO;
            return -1;
        }
        if (!chunk.data || chunk.length == 0) {
            errno = EIO;
            return -1;
        }

        char prefix[32];
        int count = snprintf(prefix, sizeof(prefix), "%zx\r\n", chunk.length);
        if (count < 0 || (size_t)count >= sizeof(prefix)) {
            errno = EOVERFLOW;
            return -1;
        }

        if (send_bytes(fd, prefix, (size_t)count, io) < 0) return -1;
        if (send_bytes(fd, chunk.data, chunk.length, io) < 0) return -1;
        if (send_bytes(fd, "\r\n", 2, io) < 0) return -1;
    }
}

/* 상태 줄·Content-Type·Allow를 보내고, 준비된 추가 헤더가 있으면 이어서 보낸다.
 * 길이·연결 헤더는 마지막에 붙여, 프록시가 만든 헤더와 중복되지 않게 한다. */
static int send_response(int fd, int status, const char *type,
                         const unsigned char *extra_headers, size_t extra_length,
                         struct body_provider *provider, size_t length,
                         bool chunked, const char *allow,
                         bool head_request, const struct io_context *io) {
    char header[512];
    char allow_line[32] = "";
    if (allow) {
        int count = snprintf(allow_line, sizeof(allow_line), "Allow: %s\r\n", allow);
        if (count < 0 || (size_t)count >= sizeof(allow_line)) { errno = EOVERFLOW; return -1; }
    }

    /* 204·304는 본문을 가질 수 없고 204는 Content-Length도 보내면 안 된다. */
    bool bodiless_status = status == 204 || status == 304;
    int count = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%s",
                         status, http_reason(status), type, allow_line);
    if (count < 0 || (size_t)count >= sizeof(header)) { errno = EOVERFLOW; return -1; }
    if (send_bytes(fd, header, (size_t)count, io) < 0) return -1;
    if (extra_length && send_bytes(fd, extra_headers, extra_length, io) < 0) return -1;

    if (head_request || bodiless_status) {
        /* HEAD(501)와 본문 없는 상태는 헤더까지만 보내고 내용을 붙이지 않는다. */
        count = snprintf(header, sizeof(header), "Connection: close\r\n\r\n");
    } else if (chunked) {
        count = snprintf(header, sizeof(header),
                         "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
    } else {
        count = snprintf(header, sizeof(header),
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n", length);
    }
    if (count < 0 || (size_t)count >= sizeof(header)) { errno = EOVERFLOW; return -1; }
    if (send_bytes(fd, header, (size_t)count, io) < 0) return -1;

    if (head_request || bodiless_status) return 0;
    return chunked ? send_chunked_body(fd, provider, io)
                   : send_body(fd, provider, length, io);
}

/* Takes responsibility for provider cleanup, including on send failure. */
static int send_prepared_response(int fd, struct router_response *response,
                                  bool head_request, const struct io_context *io) {
    int result = send_response(fd, response->status, response->content_type,
                               response->headers, response->headers_length,
                               &response->body_provider, response->length,
                               response->chunked, response->allow, head_request, io);
    int send_errno = errno;

    if (response->body_provider.cleanup_func)
        response->body_provider.cleanup_func(response->body_provider.state);
    if (result < 0) errno = send_errno;
    return result;
}

/* Half-close write side and drain bounded leftover client input before closing. */
static void finish_response(int fd, const struct io_context *io) {
    if (!io_is_active(io) || shutdown(fd, SHUT_WR) < 0) return;

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) return;
    deadline.tv_sec += DRAIN_DEADLINE_SECONDS;

    struct io_context drain = {io->running, &deadline};
    unsigned char discard[1024];
    size_t discarded = 0;
    while (io_is_active(&drain) && discarded < DRAIN_MAX_BYTES) {
        size_t room = DRAIN_MAX_BYTES - discarded;
        if (room > sizeof(discard)) room = sizeof(discard);
        ssize_t count = receive_some(fd, discard, room, &drain);
        if (count <= 0) break;
        discarded += (size_t)count;
    }
}

void handle_client(int connfd, const volatile sig_atomic_t *running,
                   const struct timespec *accepted_at) {
    unsigned char header[REQ_HEADER_MAX_BYTES];
    unsigned char *body = NULL;
    struct http_request request = {0};
    router_handler route_handler_func = NULL;
    const char *allow = NULL;
    size_t received = 0, header_end = 0, body_received = 0;
    bool head_request = false;
    struct timespec header_deadline = *accepted_at;
    header_deadline.tv_sec += HEADER_DEADLINE_SECONDS;
    struct io_context input = {running, &header_deadline};
    const struct io_context output = {running, NULL};
    int status = 0;

    if (set_fd_close_on_exec(connfd) < 0) {
        perror("client cloexec");
        goto cleanup;
    }

    int result = receive_header(connfd, header, sizeof(header),
                                &received, &header_end, &input);
    if (!io_is_active(&input)) goto cleanup;
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
    status = http_select_handler(&request, &route_handler_func, &allow);
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
            if (io_is_active(&output)) perror("send 100 Continue");
            goto cleanup;
        }
    }

    input.deadline = &body_deadline;
    while (body_received < request.content_length) {
        ssize_t count = receive_some(connfd, body + body_received,
                                     request.content_length - body_received, &input);
        if (!io_is_active(&input)) goto cleanup;
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
    if (io_is_active(&output)) {
        struct router_response response;
        if (status == 200 && route_handler_func) route_handler_func(&request, &response);
        else router_prepare_error(&response, status, allow);

        int send_result = send_prepared_response(connfd, &response, head_request, &output);
        int send_errno = errno;
        if (send_result == 0) {
            finish_response(connfd, &output);
        } else if (io_is_active(&output)) {
            errno = send_errno;
            perror("send response");
        }
    }
cleanup:
    /* Sole owner of body storage; input/output helpers never release it. */
    free(body);
}
