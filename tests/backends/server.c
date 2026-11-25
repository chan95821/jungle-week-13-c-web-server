#define _POSIX_C_SOURCE 200809L
/* Test-only backend: reuse the production C listener, parser and socket helpers. */
#include "../../src/socket_io.h"
#include "../../src/http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t backend_running = 1;
static void stop_backend(int signal_number) { (void)signal_number; backend_running = 0; }
static void handle_backend_request(int, const char *, const char *);
static void pause_backend(int);
static void pause_backend_milliseconds(long);
static bool find_request_field(const struct http_request *, const char *, struct http_slice *);

int main(int argc, char **argv) {
    uint16_t port;
    if ((argc != 3 && argc != 4) || parse_port(argv[1], &port) < 0 ||
        (strcmp(argv[2], "submission") && strcmp(argv[2], "query-a") &&
         strcmp(argv[2], "query-b"))) {
        fprintf(stderr, "Usage: %s <port> submission|query-a|query-b [mode]\n", argv[0]);
        return 1;
    }
    struct sigaction stop = {.sa_handler = stop_backend};
    if (sigemptyset(&stop.sa_mask) < 0 || sigaction(SIGINT, &stop, NULL) < 0 ||
        sigaction(SIGTERM, &stop, NULL) < 0) return 1;
    stop.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &stop, NULL) < 0) return 1;
    int listener = open_listenfd(port);
    if (listener < 0) return 1;
    while (backend_running) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) continue;
            perror("test backend accept");
            close(listener);
            return 1;
        }
        if (backend_running)
            handle_backend_request(client, argv[2], argc == 4 ? argv[3] : "normal");
        close(client);
    }
    close(listener);
    return 0;
}

static void handle_backend_request(int client, const char *identity, const char *mode) {
    unsigned char header[REQ_HEADER_MAX_BYTES];
    size_t received = 0, header_end = 0;
    unsigned char *body = NULL;
    struct timespec deadline;
    if (set_close_on_exec(client) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) return;
    deadline.tv_sec += 15; /* Test fixture request deadline; no response lifetime cap. */
    struct io_context io = {&backend_running, &deadline};
    while (received < sizeof(header) && !header_end) {
        ssize_t count = receive_with_deadline(client, header + received,
                                               sizeof(header) - received, &io);
        if (count <= 0) return;
        size_t start = received > 3 ? received - 3 : 0;
        received += (size_t)count;
        for (size_t i = start; i + 4 <= received; ++i) {
            if (!memcmp(header + i, "\r\n\r\n", 4)) { header_end = i + 4; break; }
        }
    }
    struct http_request request;
    if (!header_end || http_parse_request(header, header_end, &request)) return;
    if (request.content_length) {
        body = malloc(request.content_length);
        if (!body) return;
        size_t copied = received - header_end;
        if (copied > request.content_length) copied = request.content_length;
        memcpy(body, header + header_end, copied);
        while (copied < request.content_length) {
            ssize_t count = receive_with_deadline(client, body + copied,
                                                   request.content_length - copied, &io);
            if (count <= 0) { free(body); return; }
            copied += (size_t)count;
        }
    }
    request.body = (struct http_slice){body, request.content_length};
    io.deadline = NULL;
    if (!strcmp(mode, "large-body")) {
        static const char header[] = "HTTP/1.1 200 OK\r\nContent-Length: 4194304\r\n\r\n";
        unsigned char bytes[16384];
        for (size_t i=0; i<sizeof(bytes); ++i) bytes[i]=(unsigned char)(i%251);
        if (send_all_with_deadline(client,header,sizeof(header)-1,&io)<0) goto cleanup;
        for (size_t sent=0; sent<4194304; sent+=sizeof(bytes))
            if (send_all_with_deadline(client,bytes,sizeof(bytes),&io)<0) break;
        goto cleanup;
    }
    if (!strcmp(mode, "notify-silent")) {
        fputs("request-received\n", stderr);
        fflush(stderr);
        pause_backend(3);
        goto cleanup;
    }
    if (!strcmp(mode, "delayed-header")) pause_backend(2);
    if (!strcmp(mode, "partial-header")) {
        static const char partial[] = "HTTP/1.1 200 OK\r\nX-Partial: ";
        if (send_all_with_deadline(client, partial, sizeof(partial) - 1, &io) == 0)
            pause_backend(3);
        goto cleanup;
    }
    if (!strcmp(mode, "slow-header")) {
        static const char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        for (size_t offset = 0; offset < sizeof(wire) - 1 && backend_running; offset += 4) {
            size_t count = sizeof(wire) - 1 - offset;
            if (count > 4) count = 4;
            if (send_all_with_deadline(client, wire + offset, count, &io) < 0) break;
            pause_backend_milliseconds(400);
        }
        goto cleanup;
    }
    if (!strcmp(mode, "slow-body")) {
        static const char header[] =
            "HTTP/1.1 201 Created\r\nContent-Type: application/octet-stream\r\n"
            "Content-Length: 2000\r\n\r\n";
        unsigned char chunk[250];
        memset(chunk, 'S', sizeof(chunk));
        if (send_all_with_deadline(client, header, sizeof(header) - 1, &io) < 0)
            goto cleanup;
        for (size_t offset = 0; offset < 2000 && backend_running; offset += sizeof(chunk)) {
            if (send_all_with_deadline(client, chunk, sizeof(chunk), &io) < 0) break;
            if (offset + sizeof(chunk) < 2000) pause_backend_milliseconds(300);
        }
        goto cleanup;
    }
    if (!strcmp(mode, "silent")) { pause_backend(3); goto cleanup; }
    if (!strcmp(mode, "reset")) {
        struct linger reset = {.l_onoff = 1, .l_linger = 0};
        (void)setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        goto cleanup;
    }
    static const struct {
        const char *mode;
        const char *header;
        const void *body;
        size_t length;
    } legacy_responses[] = {
        {"http10", "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\n",
         "hello", sizeof("hello") - 1},
        {"http10-close", "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n",
         "close\0body", sizeof("close\0body") - 1},
        {"http10-truncate", "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n",
         "short", sizeof("short") - 1},
        {"http10-custom", "HTTP/1.0 409 Legacy conflict\r\nContent-Length: 8\r\n\r\n",
         "conflict", sizeof("conflict") - 1},
        {"http10-no-content", "HTTP/1.0 204 No Content\r\n\r\n", "", 0},
        {"http10-not-modified", "HTTP/1.0 304 Not Modified\r\nContent-Length: 123\r\n\r\n", "", 0},
        {"http10-informational", "HTTP/1.0 103 Early Hints\r\n\r\n", "", 0},
        {"http10-te", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
         "0\r\n\r\n", sizeof("0\r\n\r\n") - 1}
    };
    for (size_t i = 0; i < sizeof(legacy_responses) / sizeof(legacy_responses[0]); ++i) {
        if (strcmp(mode, legacy_responses[i].mode)) continue;
        if (send_all_with_deadline(client, legacy_responses[i].header,
                                   strlen(legacy_responses[i].header), &io) == 0)
            (void)send_all_with_deadline(client, legacy_responses[i].body,
                                         legacy_responses[i].length, &io);
        goto cleanup;
    }
    if (!strcmp(mode, "bad-length")) {
        static const char invalid[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx";
        (void)send_all_with_deadline(client, invalid, sizeof(invalid) - 1, &io);
        goto cleanup;
    }
    if (!strcmp(mode, "chunked") || !strcmp(mode, "bad-chunk")) {
        static const char valid[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
            "Content-Type: application/octet-stream\r\nTrailer: X-End\r\n\r\n"
            "3;kind=\"test\"\r\na\0b\r\n2\r\ncd\r\n0\r\nX-End: complete\r\n\r\n";
        static const char invalid[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\nZ\r\n";
        const char *wire = !strcmp(mode, "chunked") ? valid : invalid;
        size_t length = !strcmp(mode, "chunked") ? sizeof(valid) - 1 : sizeof(invalid) - 1;
        (void)send_all_with_deadline(client, wire, length, &io);
        goto cleanup;
    }
    if (!strcmp(mode, "close")) {
        static const char wire[] =
            "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\nclose\0body";
        (void)send_all_with_deadline(client, wire, sizeof(wire) - 1, &io);
        goto cleanup;
    }
    if (!strcmp(mode, "truncate") || !strcmp(mode, "body-timeout") ||
        !strcmp(mode, "body-total")) {
        static const char wire[] = "HTTP/1.1 200 OK\r\nContent-Length: 128\r\n\r\npartial";
        if (send_all_with_deadline(client, wire, sizeof(wire) - 1, &io) < 0) goto cleanup;
        if (!strcmp(mode, "body-timeout")) pause_backend(6);
        if (!strcmp(mode, "body-total")) {
            for (int i = 0; i < 8 && backend_running; ++i) {
                pause_backend(2);
                if (send_all_with_deadline(client, "x", 1, &io) < 0) break;
            }
        }
        goto cleanup;
    }
    if (!strcmp(mode, "no-content") || !strcmp(mode, "not-modified")) {
        const char *wire = !strcmp(mode, "no-content")
            ? "HTTP/1.1 204 No Content\r\n\r\n"
            : "HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n";
        (void)send_all_with_deadline(client, wire, strlen(wire), &io);
        goto cleanup;
    }
    if (!strcmp(mode, "informational")) {
        static const char interim[] = "HTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\n";
        if (send_all_with_deadline(client, interim, sizeof(interim) - 1, &io) < 0) goto cleanup;
    }

    bool submission = !strcmp(identity, "submission");
    bool correct_route = submission
        ? http_slice_equal(request.method, "POST") &&
          http_slice_equal(request.path, "/api/submissions")
        : http_slice_equal(request.method, "GET") &&
          (http_slice_equal(request.path, "/api/submission-status") ||
           http_slice_equal(request.path, "/api/hello"));
    static const unsigned char status_body[] = "status\0ready\xff";
    static const unsigned char hello[] = "Hello";
    const unsigned char *response_body = submission ? body :
        http_slice_equal(request.path, "/api/hello") ? hello : status_body;
    size_t response_length = submission ? request.body.length :
        http_slice_equal(request.path, "/api/hello") ? sizeof(hello) - 1 : sizeof(status_body) - 1;
    unsigned char benchmark_body[1024];
    if (!strcmp(mode, "benchmark") && !submission && correct_route) {
        for (size_t i = 0; i < sizeof(benchmark_body); ++i) benchmark_body[i] = (unsigned char)i;
        response_body = benchmark_body;
        response_length = sizeof(benchmark_body);
    }
    if (!correct_route) { response_body = (const unsigned char *)"wrong backend"; response_length = 13; }
    struct http_slice content_type = {0}, trace = {0}, hop = {0}, expectation = {0};
    struct http_slice received_length = {(const unsigned char *)"absent", sizeof("absent") - 1};
    (void)find_request_field(&request, "content-length", &received_length);
    (void)find_request_field(&request, "content-type", &content_type);
    (void)find_request_field(&request, "x-trace", &trace);
    bool has_hop = find_request_field(&request, "x-hop", &hop);
    bool has_expect = find_request_field(&request, "expect", &expectation);
    char response_header[2 * REQ_HEADER_MAX_BYTES + 512];
    int count = snprintf(response_header, sizeof(response_header),
        "HTTP/1.1 %s\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\nX-Backend-ID: %s\r\n"
        "X-Received-Host: %.*s\r\nX-Received-Target: %.*s\r\n"
        "X-Received-Content-Length: %.*s\r\n"
        "X-Received-Content-Type: %.*s\r\nX-Received-Trace: %.*s\r\n"
        "X-Received-Hop: %s\r\nX-Received-Expect: %s\r\n"
        "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
        "Connection: close, X-Backend-Hop\r\nX-Backend-Hop: remove\r\n\r\n",
        !correct_route ? "404 Not Found" : submission ? "201 Created" : "200 OK",
        response_length, identity, (int)request.host.length, request.host.data,
        (int)request.target.length, request.target.data,
        (int)received_length.length, received_length.data,
        (int)content_type.length, content_type.length ? content_type.data : (const unsigned char *)"",
        (int)trace.length, trace.length ? trace.data : (const unsigned char *)"",
        has_hop ? "present" : "absent", has_expect ? "present" : "absent");
    if (count < 0 || (size_t)count >= sizeof(response_header)) goto cleanup;
    if (send_all_with_deadline(client, response_header, (size_t)count, &io) == 0)
        (void)send_all_with_deadline(client, response_body, response_length, &io);
cleanup:
    free(body);
}

static void pause_backend(int seconds) {
    pause_backend_milliseconds((long)seconds * 1000);
}

static void pause_backend_milliseconds(long milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    while (backend_running && nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static bool find_request_field(const struct http_request *request, const char *name,
                                struct http_slice *value) {
    size_t offset = 0;
    struct http_field field;
    while (http_next_field(request->headers, &offset, &field) > 0) {
        if (http_field_name_equal(field.name, name)) { *value = field.value; return true; }
    }
    return false;
}