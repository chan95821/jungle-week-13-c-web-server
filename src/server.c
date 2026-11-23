#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "router.h"
#include "static_file.h"
#include "dynamic.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
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

/* 메모리 응답과 파일 응답이 공통으로 사용하는 헤더 작성/전송 함수다.
 * 파일 크기는 off_t에서 uintmax_t로 전달해 size_t로 조용히 잘리지 않게 한다. */
static int send_response_header(int fd, int status, const char *type,
                                uintmax_t length, const char *allow,
                                bool head_request, const struct io_context *io) {
    char header[512];
    char allow_line[32] = "";
    if (allow) {
        int count = snprintf(allow_line, sizeof(allow_line), "Allow: %s\r\n", allow);
        if (count < 0 || (size_t)count >= sizeof(allow_line)) { errno = EOVERFLOW; return -1; }
    }
    int count;
    if (head_request) {
        /* HEAD is unsupported (501), but must not carry response content. */
        count = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%sConnection: close\r\n\r\n",
                         status, http_reason(status), type, allow_line);
    } else {
        count = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %" PRIuMAX "\r\n"
                         "%sConnection: close\r\n\r\n",
                         status, http_reason(status), type, length, allow_line);
    }
    if (count < 0 || (size_t)count >= sizeof(header)) { errno = EOVERFLOW; return -1; }
    return send_bytes(fd, header, (size_t)count, io);
}

static int send_response(int fd, int status, const char *type,
                         const void *body, size_t length, const char *allow,
                         bool head_request, const struct io_context *io) {
    if (send_response_header(fd, status, type, (uintmax_t)length, allow, head_request, io) < 0) return -1;
    return head_request ? 0 : send_bytes(fd, body, length, io);
}

static int send_file_body(int connfd, const struct static_file *file,
                          const struct io_context *io) {
    /* 파일 전체 malloc 대신 16KiB 버퍼 하나를 반복 사용한다.
     * read가 적게 읽거나 send가 적게 보냈어도 각 실제 길이만 진행시킨다.
     * 전송 크기는 요청 본문 64KiB 상한과 관계없다. */
    unsigned char buffer[16 * 1024];
    uintmax_t remaining = file->length;
    while (remaining > 0) {
        if (!active(io)) { errno = ECANCELED; return -1; }
        size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t count = read(file->fd, buffer, wanted);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) { errno = EIO; return -1; }
        /* 송신 타임아웃과 종료 표시를 유지하는 기존 send_bytes를 재사용한다.
         * 이미 200 헤더를 보냈으므로 여기서 실패하면 새 500을 붙이지 않고 종료한다. */
        if (send_bytes(connfd, buffer, (size_t)count, io) < 0) return -1;
        remaining -= (uintmax_t)count;
    }
    /* 전송 도중 파일이 커져도 처음 선언한 Content-Length 뒤의 바이트는 보내지 않는다.
     * 파일 내용이 제자리에서 수정되는 상황까지 스냅샷 일관성을 보장하지는 않는다. */
    return 0;
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
    struct http_request request = {0};
    struct static_file file = {.fd = -1};
    /* 동적 함수가 만든 JSON은 이 버퍼에 놓인다. 함수 호출이 끝나도 handle_client가
     * 반환하기 전까지 유효하므로 send_response가 안전하게 읽을 수 있다. */
    char dynamic_body[DYNAMIC_RESPONSE_CAPACITY];
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
    if (route == HTTP_ROUTE_TIME || route == HTTP_ROUTE_ADD) {
        status = dynamic_check_headers(&request, route);
        if (status) goto respond;
    }
    /* 파일 확인은 최종 200 헤더 및 100 Continue 전에 한다.
     * 없는 파일/금지 경로라면 본문을 더 보내도록 요구하지 않고 오류로 응답한다.
     * 성공한 GET도 선언한 요청 본문이 있으면 기존 정책대로 끝까지 수신한다. */
    if (route == HTTP_ROUTE_STATIC) {
        status = prepare_static_file(rootfd, request.path, &file);
        if (status) goto respond;
    }

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
        int sent;
        if (status == 200 && route == HTTP_ROUTE_STATIC) {
            sent = send_response_header(connfd, 200, file.content_type,
                                        file.length, NULL, false, &output);
            if (sent == 0) sent = send_file_body(connfd, &file, &output);
        } else {
            struct router_response response;
            if (status == 200 && (route == HTTP_ROUTE_TIME || route == HTTP_ROUTE_ADD)) {
                /* 요청 본문이 완성된 뒤에만 계산한다. 오류면 기존 오류 응답으로 바꾸고
                 * 정상일 때만 호출자 소유 버퍼를 가리키는 JSON 응답을 전송한다. */
                status = prepare_dynamic_response(&request, route, dynamic_body,
                                                   sizeof(dynamic_body), &response);
                if (status) response = router_prepare_response(&request, route, status, allow);
            } else {
                response = router_prepare_response(&request, route, status, allow);
            }
            sent = send_response(connfd, response.status, response.content_type,
                                 response.body, response.length, response.allow,
                                 head_request, &output);
        }
        if (sent == 0) {
            finish_response(connfd, &output);
        } else if (active(&output)) {
            perror("send response");
        }
    }
cleanup:
    /* 파일 FD는 여기서만 닫고, 루트 FD는 main에 빌렸으므로 닫지 않는다.
     * 파싱 실패·본문 미완성·전송 실패·종료 시그널도 모두 이 경로로 모인다. */
    if (file.fd >= 0 && close(file.fd) < 0) perror("close static file");
    /* Sole owner of body storage; input/output helpers never release it. */
    free(body);
}
