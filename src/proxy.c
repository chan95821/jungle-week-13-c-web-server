#define _POSIX_C_SOURCE 200809L
#include "proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { BACKEND_TIMEOUT_MS = 1000, CLIENT_TIMEOUT_MS = 5000,
       PROXY_BUFFER_BYTES = 8192 };

/* CS:APP의 robust I/O와 같은 목적의 직접 구현이다. TCP는 바이트 순서와
 * 재전송을 맡지만 HTTP 경계, 부분 recv/send 누적, timeout은 서버 책임이다.
 * reader는 한 연결의 함수 스택에만 있다. 헤더와 같이 도착한 본문도 보관한다. */
struct reader {
    int fd;
    const volatile sig_atomic_t *running;
    unsigned char buffer[PROXY_BUFFER_BYTES];
    size_t begin, end;
};

struct field {
    struct http_slice line, name, value;
};

static bool alive(const volatile sig_atomic_t *running) {
    return !running || *running;
}

static int64_t now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* EINTR마다 timeout을 새로 시작하지 않는다. nonblocking connect의 완료도
 * poll로 기다리지만, 이는 여러 클라이언트를 처리하는 이벤트 루프는 아니다. */
static int wait_until(int fd, short events, int64_t deadline,
                       const volatile sig_atomic_t *running) {
    for (;;) {
        if (!alive(running)) { errno = ECANCELED; return -1; }
        int64_t now = now_ms();
        if (now < 0) return -1;
        int64_t left = deadline - now;
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd item = {.fd = fd, .events = events};
        int result = poll(&item, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (!alive(running)) { errno = ECANCELED; return -1; }
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (!result) { errno = ETIMEDOUT; return -1; }
        if (item.revents & POLLNVAL) { errno = EBADF; return -1; }
        /* HUP에서도 recv를 호출해야 이미 도착한 마지막 바이트를 읽을 수 있다. */
        return 0;
    }
}

static ssize_t read_backend(struct reader *source) {
    int64_t start = now_ms();
    if (start < 0) return -1;
    for (;;) {
        if (wait_until(source->fd, POLLIN, start + BACKEND_TIMEOUT_MS,
                       source->running) < 0) return -1;
        ssize_t count = recv(source->fd, source->buffer, sizeof(source->buffer), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count >= 0) { source->begin = 0; source->end = (size_t)count; }
        return count;
    }
}

/* CS:APP rio_writen과 유사한 완전 송신. send가 short count를 반환할 수 있다.
 * 진전이 있을 때만 무진행 제한을 갱신한다. 전체 응답 제한 시간은 아니다. */
static int write_all(int fd, const void *data, size_t length, int timeout_ms,
                      const volatile sig_atomic_t *running) {
    const unsigned char *bytes = data;
    int64_t start = now_ms();
    if (start < 0) return -1;
    while (length) {
        if (wait_until(fd, POLLOUT, start + timeout_ms, running) < 0) return -1;
        ssize_t count = send(fd, bytes, length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) { if (!count) errno = EIO; return -1; }
        bytes += count;
        length -= (size_t)count;
        start = now_ms();
        if (start < 0) return -1;
    }
    return 0;
}

int proxy_parse_endpoint(const char *text, struct sockaddr_in *address) {
    const char *colon = strchr(text, ':');
    char ip[INET_ADDRSTRLEN];
    if (!colon || colon == text || (size_t)(colon - text) >= sizeof(ip)) return -1;
    memcpy(ip, text, (size_t)(colon - text));
    ip[colon - text] = '\0';
    unsigned int port = 0;
    if (!colon[1]) return -1;
    for (const char *p = colon + 1; *p; ++p) {
        if (*p < '0' || *p > '9' || port > (65535U - (unsigned int)(*p - '0')) / 10U) return -1;
        port = port * 10U + (unsigned int)(*p - '0');
    }
    *address = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    return port && inet_pton(AF_INET, ip, &address->sin_addr) == 1 ? 0 : -1;
}

int proxy_select_route(const struct http_request *request, const char **allow) {
    const char *method;
    int route;
    if (http_slice_equal(request->path, "/api/submissions")) {
        method = "POST"; route = PROXY_SUBMISSION;
    } else if (http_slice_equal(request->path, "/api/submission-status")) {
        method = "GET"; route = PROXY_STATUS;
    } else return PROXY_LOCAL;
    if (http_slice_equal(request->method, method)) return route;
    if (!http_slice_equal(request->method, "GET") && !http_slice_equal(request->method, "POST")) return -501;
    *allow = method;
    return -405;
}

/* 목적지는 클라이언트 입력이 아닌 main에서 검증한 설정에서만 선택한다. */
static int connect_backend(const struct sockaddr_in *address,
                            const volatile sig_atomic_t *running) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) goto fail;
    int64_t start = now_ms();
    if (start < 0) goto fail;
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) < 0) {
        if (errno != EINPROGRESS && errno != EINTR) goto fail;
        if (wait_until(fd, POLLOUT, start + BACKEND_TIMEOUT_MS, running) < 0) goto fail;
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) goto fail;
        if (error) { errno = error; goto fail; }
    }
    return fd;
fail: {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
}

static bool equal(struct http_slice value, const char *text) {
    return value.length == strlen(text) &&
           strncasecmp((const char *)value.data, text, value.length) == 0;
}

static bool token(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

static struct http_slice trim(struct http_slice value) {
    while (value.length && (value.data[0] == ' ' || value.data[0] == '\t')) {
        ++value.data; --value.length;
    }
    while (value.length && (value.data[value.length - 1] == ' ' || value.data[value.length - 1] == '\t')) --value.length;
    return value;
}

static size_t line_end(const unsigned char *data, size_t length) {
    for (size_t i = 0; i + 1 < length; ++i)
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    return SIZE_MAX;
}

/* 반환한 slice는 원본 헤더를 빌린다. obs-fold, 잘못된 이름·제어 문자는 거절. */
static bool next_field(const unsigned char *header, size_t length,
                         size_t *offset, struct field *field) {
    if (*offset >= length) return false;
    size_t end = line_end(header + *offset, length - *offset);
    if (end == SIZE_MAX || !end) return false;
    const unsigned char *line = header + *offset;
    size_t colon = 0;
    while (colon < end && token(line[colon])) ++colon;
    if (!colon || colon == end || line[colon] != ':') return false;
    for (size_t i = colon + 1; i < end; ++i)
        if ((line[i] < 32 && line[i] != '\t') || line[i] == 127) return false;
    field->line = (struct http_slice){line, end + 2};
    field->name = (struct http_slice){line, colon};
    field->value = trim((struct http_slice){line + colon + 1, end - colon - 1});
    *offset += end + 2;
    return true;
}

/* RFC 9110 §7.6.1: Connection의 값은 다음 연결로 넘기지 않을 필드 이름이다.
 * Connection보다 먼저 나온 필드도 제거해야 하므로 원본 전체를 재탐색한다. */
static bool connection_names(const unsigned char *header, size_t length,
                               struct http_slice name) {
    size_t offset = line_end(header, length) + 2;
    struct field field;
    while (next_field(header, length, &offset, &field)) {
        if (!equal(field.name, "connection")) continue;
        size_t i = 0;
        while (i < field.value.length) {
            size_t start = i;
            while (i < field.value.length && field.value.data[i] != ',') ++i;
            struct http_slice option = trim((struct http_slice){field.value.data + start, i - start});
            if (option.length == name.length &&
                strncasecmp((const char *)option.data, (const char *)name.data, name.length) == 0) return true;
            if (i < field.value.length) ++i;
        }
    }
    return false;
}

static bool hop_by_hop(struct http_slice name) {
    return equal(name, "connection") || equal(name, "keep-alive") ||
           equal(name, "proxy-connection") || equal(name, "proxy-authenticate") ||
           equal(name, "proxy-authorization") || equal(name, "te") ||
           equal(name, "trailer") || equal(name, "transfer-encoding") || equal(name, "upgrade");
}

static bool append(unsigned char *out, size_t *used, const void *data, size_t count) {
    if (count > PROXY_BUFFER_BYTES - *used) return false;
    memcpy(out + *used, data, count);
    *used += count;
    return true;
}

/* 원본 target(query 포함), Host, 일반 헤더를 보존한다. 본문은 이미 받은
 * 요청의 정확한 길이만 보낸다. Expect는 프록시가 처리했으므로 제거한다. */
static bool build_request(const unsigned char *header, size_t length,
                            const struct http_request *request,
                            unsigned char *out, size_t *used) {
    *used = 0;
    size_t first = line_end(header, length) + 2;
    if (!append(out, used, header, first)) return false;
    size_t offset = first;
    struct field field;
    while (next_field(header, length, &offset, &field)) {
        if (hop_by_hop(field.name) || equal(field.name, "expect") ||
            equal(field.name, "content-length") || connection_names(header, length, field.name)) continue;
        if (!append(out, used, field.line.data, field.line.length)) return false;
    }
    /* Connection: Host 같은 요청은 전달 계약을 바꾸므로 중계 전에 400. */
    if (connection_names(header, length, (struct http_slice){(const unsigned char *)"Host", 4})) return false;
    char framing[96];
    int count = snprintf(framing, sizeof(framing),
                         "Content-Length: %zu\r\nConnection: close\r\nVia: 1.1 jungle-proxy\r\n\r\n", request->body.length);
    return count > 0 && (size_t)count < sizeof(framing) && append(out, used, framing, (size_t)count);
}

static int read_byte(struct reader *source, unsigned char *byte) {
    if (source->begin == source->end) {
        ssize_t count = read_backend(source);
        if (count <= 0) { if (!count) errno = EPROTO; return -1; }
    }
    *byte = source->buffer[source->begin++];
    return 0;
}

static int read_line(struct reader *source, unsigned char *line, size_t cap, size_t *length) {
    *length = 0;
    while (*length < cap) {
        if (read_byte(source, line + *length) < 0) return -1;
        ++*length;
        if (*length >= 2 && line[*length - 2] == '\r' && line[*length - 1] == '\n') return 0;
    }
    errno = EPROTO;
    return -1;
}

static int read_header(struct reader *source, unsigned char *header, size_t *length) {
    *length = 0;
    while (*length < PROXY_BUFFER_BYTES) {
        size_t line_length;
        if (read_line(source, header + *length, PROXY_BUFFER_BYTES - *length, &line_length) < 0) return -1;
        *length += line_length;
        if (line_length == 2) return 0;
    }
    errno = EPROTO;
    return -1;
}

static bool decimal(struct http_slice text, size_t *value) {
    *value = 0;
    if (!text.length) return false;
    for (size_t i = 0; i < text.length; ++i) {
        unsigned char c = text.data[i];
        if (c < '0' || c > '9' || *value > (SIZE_MAX - (size_t)(c - '0')) / 10) return false;
        *value = *value * 10 + (size_t)(c - '0');
    }
    return true;
}

struct response_meta {
    int status;
    bool has_length, chunked;
    size_t length, first;
};

/* 최소 응답 파서. 1xx는 내부에서 소비하고 최종 상태 줄은 원문 보존한다.
 * TE+CL/중복 CL을 거절해 서로 다른 메시지 경계 해석을 피한다. */
static bool parse_response(const unsigned char *header, size_t length,
                             struct response_meta *meta) {
    *meta = (struct response_meta){0};
    size_t first = line_end(header, length);
    if (first == SIZE_MAX || first < 13 ||
        (memcmp(header, "HTTP/1.1 ", 9) && memcmp(header, "HTTP/1.0 ", 9)) ||
        header[9] < '1' || header[9] > '5' || header[10] < '0' || header[10] > '9' ||
        header[11] < '0' || header[11] > '9' || header[12] != ' ') return false;
    for (size_t i = 13; i < first; ++i)
        if ((header[i] < 32 && header[i] != '\t') || header[i] == 127) return false;
    meta->status = (header[9] - '0') * 100 + (header[10] - '0') * 10 + header[11] - '0';
    meta->first = first + 2;
    size_t offset = meta->first;
    struct field field;
    while (offset < length - 2) {
        if (!next_field(header, length, &offset, &field)) return false;
        if (equal(field.name, "content-length")) {
            if (meta->has_length || !decimal(field.value, &meta->length)) return false;
            meta->has_length = true;
        } else if (equal(field.name, "transfer-encoding")) {
            if (meta->chunked || !equal(field.value, "chunked")) return false;
            meta->chunked = true;
        } else if (equal(field.name, "connection")) {
            size_t i = 0;
            while (i < field.value.length) {
                while (i < field.value.length && (field.value.data[i] == ' ' || field.value.data[i] == '\t')) ++i;
                size_t start = i;
                while (i < field.value.length && token(field.value.data[i])) ++i;
                if (start == i) return false;
                while (i < field.value.length && (field.value.data[i] == ' ' || field.value.data[i] == '\t')) ++i;
                if (i == field.value.length) break;
                if (field.value.data[i++] != ',' || i == field.value.length) return false;
            }
            if (!field.value.length) return false;
        }
    }
    if (offset != length - 2 || (meta->has_length && meta->chunked)) return false;
    if ((meta->status < 200 || meta->status == 204) && (meta->has_length || meta->chunked)) return false;
    return meta->status != 101;
}

static bool build_response(const unsigned char *header, size_t length,
                             const struct response_meta *meta,
                             unsigned char *out, size_t *used) {
    *used = 0;
    if (!append(out, used, header, meta->first)) return false;
    size_t offset = meta->first;
    struct field field;
    while (next_field(header, length, &offset, &field)) {
        if (hop_by_hop(field.name) || equal(field.name, "content-length") ||
            connection_names(header, length, field.name)) continue;
        if (!append(out, used, field.line.data, field.line.length)) return false;
    }
    char framing[128];
    int count;
    if (meta->status == 204 || meta->status == 304) {
        /* 304의 CL은 표현 메타데이터이므로 유지할 수 있지만 실제 본문은 없다. */
        if (meta->has_length) count = snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", meta->length);
        else count = 0;
    } else if (meta->has_length) count = snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", meta->length);
    else count = snprintf(framing, sizeof(framing), "Transfer-Encoding: chunked\r\n");
    if (count < 0 || (size_t)count >= sizeof(framing) || !append(out, used, framing, (size_t)count)) return false;
    static const char end[] = "Connection: close\r\nVia: 1.1 jungle-proxy\r\n\r\n";
    if (!append(out, used, end, sizeof(end) - 1)) return false;
    /* HTTP/1.0 백엔드의 EOF 응답도 클라이언트에는 HTTP/1.1 chunked로 표현. */
    if (*used >= 8) out[7] = '1';
    return true;
}

static int emit_chunk(int client, const unsigned char *bytes, size_t length,
                        const volatile sig_atomic_t *running) {
    char prefix[32];
    int count = snprintf(prefix, sizeof(prefix), "%zx\r\n", length);
    if (count < 0 || (size_t)count >= sizeof(prefix)) { errno = EOVERFLOW; return -1; }
    if (write_all(client, prefix, (size_t)count, CLIENT_TIMEOUT_MS, running) < 0 ||
        write_all(client, bytes, length, CLIENT_TIMEOUT_MS, running) < 0 ||
        write_all(client, "\r\n", 2, CLIENT_TIMEOUT_MS, running) < 0) return -1;
    return 0;
}

/* reader에 남아 있는 바이트부터 먼저 쓴다. NUL/문자열 길이는 사용하지 않는다. */
static int relay_exact(struct reader *source, int client, size_t remaining, bool chunked) {
    while (remaining) {
        if (source->begin == source->end) {
            ssize_t count = read_backend(source);
            if (count <= 0) { if (!count) errno = EPROTO; return -1; }
        }
        size_t count = source->end - source->begin;
        if (count > remaining) count = remaining;
        int result = chunked ? emit_chunk(client, source->buffer + source->begin, count, source->running)
                             : write_all(client, source->buffer + source->begin, count, CLIENT_TIMEOUT_MS, source->running);
        if (result < 0) return -1;
        source->begin += count;
        remaining -= count;
    }
    return 0;
}

/* chunk extension은 RFC 문법으로 검사하되 해석·전달하지 않는다. */
static bool chunk_size(const unsigned char *line, size_t length, size_t *size) {
    *size = 0;
    size_t i = 0;
    while (i < length) {
        unsigned char c = line[i];
        unsigned int n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else break;
        if (*size > (SIZE_MAX - n) / 16) return false;
        *size = *size * 16 + n;
        ++i;
    }
    if (!i) return false;
    while (i < length) {
        while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i == length || line[i++] != ';') return false;
        while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t start = i;
        while (i < length && token(line[i])) ++i;
        if (start == i) return false;
        while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i < length && line[i] == '=') {
            ++i;
            while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i < length && line[i] == '"') {
                ++i;
                bool closed = false;
                while (i < length) {
                    unsigned char c = line[i++];
                    if (c == '"') { closed = true; break; }
                    if (c == '\\') { if (i == length) return false; c = line[i++]; }
                    if ((c < 32 && c != '\t') || c == 127) return false;
                }
                if (!closed) return false;
            } else {
                start = i;
                while (i < length && token(line[i])) ++i;
                if (start == i) return false;
            }
        }
    }
    return true;
}

/* 백엔드 청크를 해제 후 재청크한다. 종료 청크는 모든 데이터·trailer를
 * 확인한 뒤에만 생성한다. 조기 EOF/오류면 클라이언트는 완료 표시를 못 받는다.
 * trailer는 이 초안에서 전달하지 않으며 총 8KiB까지 검증하고 소비한다. */
static int relay_chunked(struct reader *source, int client) {
    unsigned char line[PROXY_BUFFER_BYTES];
    for (;;) {
        size_t length, size;
        if (read_line(source, line, sizeof(line), &length) < 0) return -1;
        if (!chunk_size(line, length - 2, &size)) { errno = EPROTO; return -1; }
        if (!size) break;
        if (relay_exact(source, client, size, true) < 0) return -1;
        unsigned char cr, lf;
        if (read_byte(source, &cr) < 0 || read_byte(source, &lf) < 0) return -1;
        if (cr != '\r' || lf != '\n') { errno = EPROTO; return -1; }
    }
    size_t total = 0;
    for (;;) {
        size_t length;
        if (read_line(source, line, sizeof(line) - total, &length) < 0) return -1;
        total += length;
        if (length == 2) break;
        size_t offset = 0;
        struct field field;
        if (!next_field(line, length, &offset, &field) || hop_by_hop(field.name) ||
            equal(field.name, "content-length") || equal(field.name, "host")) {
            errno = EPROTO; return -1;
        }
    }
    return write_all(client, "0\r\n\r\n", 5, CLIENT_TIMEOUT_MS, source->running);
}

static int relay_until_eof(struct reader *source, int client) {
    for (;;) {
        if (source->begin != source->end) {
            if (emit_chunk(client, source->buffer + source->begin,
                           source->end - source->begin, source->running) < 0) return -1;
            source->begin = source->end;
        }
        ssize_t count = read_backend(source);
        if (count < 0) return -1;
        if (!count) return write_all(client, "0\r\n\r\n", 5, CLIENT_TIMEOUT_MS, source->running);
    }
}

static int upstream_error(void) {
    if (errno == ECANCELED) return -1;
    if (errno == ETIMEDOUT) return 504;
    if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) return 500;
    return 502;
}

int proxy_forward(int client, const struct http_request *request,
                  const unsigned char *header, size_t header_length,
                  enum proxy_route route, struct proxy_config *config,
                  const volatile sig_atomic_t *running) {
    unsigned char wire[PROXY_BUFFER_BYTES];
    size_t wire_length;
    if (!build_request(header, header_length, request, wire, &wire_length)) return 400;

    /* 한 요청에 목적지는 단 하나. 실패 시 다른 서버로 재시도하지 않는다.
     * 제출 POST의 재시도는 실제 저장이 도입되면 중복 제출을 만들 수 있다. */
    const struct sockaddr_in *address = &config->submission;
    if (route == PROXY_STATUS) {
        address = &config->status[config->next_status];
        config->next_status = (config->next_status + 1) % 2;
    }
    int backend = connect_backend(address, running);
    if (backend < 0) return upstream_error();
    int result = 502;
    int client_flags = -1;
    if (write_all(backend, wire, wire_length, BACKEND_TIMEOUT_MS, running) < 0 ||
        write_all(backend, request->body.data, request->body.length, BACKEND_TIMEOUT_MS, running) < 0) {
        result = upstream_error(); goto cleanup;
    }

    struct reader source = {.fd = backend, .running = running};
    unsigned char response_header[PROXY_BUFFER_BYTES];
    struct response_meta meta;
    size_t header_size, interim_total = 0;
    for (;;) {
        if (read_header(&source, response_header, &header_size) < 0) {
            result = upstream_error(); goto cleanup;
        }
        if (!parse_response(response_header, header_size, &meta)) goto cleanup;
        if (meta.status >= 200) break;
        /* 100/102/103은 최종 응답이 아니다. 무한 1xx 스트림도 제한한다. */
        interim_total += header_size;
        if (interim_total > PROXY_BUFFER_BYTES) goto cleanup;
    }
    if (!build_response(response_header, header_size, &meta, wire, &wire_length)) goto cleanup;

    client_flags = fcntl(client, F_GETFL);
    if (client_flags < 0 || fcntl(client, F_SETFL, client_flags | O_NONBLOCK) < 0) {
        result = 500; goto cleanup;
    }
    /* 첫 send가 일부만 성공해도 이미 응답은 시작됐을 수 있다. 이 시점부터
     * 모든 실패는 -1로만 반환해 caller가 502/504를 뒤에 붙이지 못하게 한다. */
    result = -1;
    if (write_all(client, wire, wire_length, CLIENT_TIMEOUT_MS, running) < 0) goto cleanup;
    if (meta.status == 204 || meta.status == 304) result = 0;
    else if (meta.has_length) result = relay_exact(&source, client, meta.length, false);
    else if (meta.chunked) result = relay_chunked(&source, client);
    else result = relay_until_eof(&source, client);

cleanup: {
        int error = errno;
        /* 정상·거절·timeout·잘못된 헤더·부분 응답·클라이언트 종료 모두 동일한
         * FD 정리 지점이다. client 최종 close는 main의 기존 책임으로 남긴다. */
        close(backend);
        if (client_flags >= 0 && fcntl(client, F_SETFL, client_flags) < 0) return -1;
        errno = error;
        return alive(running) ? result : -1;
    }
}
