#define _POSIX_C_SOURCE 200809L
#include "../../src/proxy_internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Only test-built proxy objects rename allocators. No production hooks/state. */
static bool track_allocations;
static long allocations_before_failure = -1;
static struct { void *pointer; size_t size; } allocations[16];
static size_t live_allocations;

static bool fail_allocation(void) {
    if (!track_allocations || allocations_before_failure < 0) return false;
    if (allocations_before_failure == 0) { errno = ENOMEM; return true; }
    --allocations_before_failure;
    return false;
}
static void remember_allocation(void *pointer, size_t size) {
    if (!track_allocations || !pointer) return;
    for (size_t i = 0; i < 16; ++i) {
        if (!allocations[i].pointer) {
            allocations[i].pointer = pointer;
            allocations[i].size = size;
            ++live_allocations;
            return;
        }
    }
    assert(!"allocation tracking capacity exceeded");
}
void *proxy_test_malloc(size_t size) {
    if (fail_allocation()) return NULL;
    void *pointer = malloc(size);
    remember_allocation(pointer, size);
    return pointer;
}
void *proxy_test_calloc(size_t count, size_t size) {
    if (fail_allocation()) return NULL;
    void *pointer = calloc(count, size);
    remember_allocation(pointer, count * size);
    return pointer;
}
void proxy_test_free(void *pointer) {
    if (pointer) {
        for (size_t i = 0; i < 16; ++i) {
            if (allocations[i].pointer == pointer) {
                allocations[i].pointer = NULL;
                --live_allocations;
                break;
            }
        }
    }
    free(pointer);
}
static size_t allocation_size(void *pointer) {
    assert(pointer);
    for (size_t i = 0; i < 16; ++i)
        if (allocations[i].pointer == pointer) return allocations[i].size;
    assert(!"allocation not tracked");
    return 0;
}
static void start_tracking(long fail_after) {
    assert(live_allocations == 0);
    track_allocations = true;
    allocations_before_failure = fail_after;
}
static void stop_tracking(void) {
    assert(live_allocations == 0);
    track_allocations = false;
    allocations_before_failure = -1;
}

/* A tiny C/POSIX backend in a separate process; parent tests public preparation. */
static pid_t start_backend(const char *wire, struct sockaddr_in *endpoint) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    *endpoint = (struct sockaddr_in){.sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    assert(bind(listener, (struct sockaddr *)endpoint, sizeof(*endpoint)) == 0);
    socklen_t length = sizeof(*endpoint);
    assert(getsockname(listener, (struct sockaddr *)endpoint, &length) == 0);
    assert(listen(listener, 1) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        alarm(5);
        int client = accept(listener, NULL, NULL);
        if (client < 0) _exit(10);
        unsigned char request[1024];
        size_t used = 0;
        while (used < sizeof(request)) {
            ssize_t count = recv(client, request + used, sizeof(request) - used, 0);
            if (count <= 0) _exit(11);
            used += (size_t)count;
            bool complete = false;
            for (size_t i = 0; i + 4 <= used; ++i)
                if (!memcmp(request + i, "\r\n\r\n", 4)) complete = true;
            if (complete) break;
        }
        int result = send_all(client, wire, strlen(wire));
        close(client);
        close(listener);
        _exit(result == 0 ? 0 : 12);
    }
    close(listener);
    return child;
}
static struct http_request get_request(void) {
    static const unsigned char request[] =
        "GET /api/hello HTTP/1.1\r\nHost: original.example\r\n\r\n";
    struct http_request parsed;
    assert(http_parse_request(request, sizeof(request) - 1, &parsed) == 0);
    return parsed;
}
static void wait_backend(pid_t child) {
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void assert_cleanup_closes_backend(struct router_response *response) {
    struct proxy_stream *stream = response->body_provider.state;
    int backend_fd = stream->backend_fd;
    response->body_provider.cleanup_func(stream);
    errno = 0;
    assert(fcntl(backend_fd, F_GETFD) == -1 && errno == EBADF);
    assert(live_allocations == 0);
}

static const char chunked_header[] =
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
    "Connection: close, X-First\r\ncOnNeCtIoN: X-Second\r\n\r\n";
static const char retained_connection[] =
    "Connection: close, X-First\r\ncOnNeCtIoN: X-Second\r\n";

static size_t count_test_descriptors(void) {
    size_t count = 0;
    /* These isolated test cases own only a handful of low-numbered descriptors. */
    for (int fd = 0; fd < 256; ++fd)
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

static void test_preparation_allocation_failures(void) {
    struct http_request request = get_request();
    struct router_response response;
    struct sockaddr_in endpoint = {.sin_family = AF_INET};
    start_tracking(0);
    proxy_prepare_response(&endpoint, &request, NULL, &response);
    assert(response.status == 500);
    stop_tracking();
    const char *headers[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", chunked_header
    };
    for (size_t kind = 0; kind < 2; ++kind) {
        /* State allocation succeeds; fail exact wire copy or Connection copy. */
        for (long fail_after = 1; fail_after <= (long)kind + 1; ++fail_after) {
            pid_t child = start_backend(headers[kind], &endpoint);
            size_t open_before = count_test_descriptors();
            start_tracking(fail_after);
            proxy_prepare_response(&endpoint, &request, NULL, &response);
            assert(response.status == 500 && response.wire_header.length == 0);
            assert(live_allocations == 0);
            assert(count_test_descriptors() == open_before);
            stop_tracking();
            wait_backend(child);
        }
    }
}

static void test_header_and_trailer_lifetimes(void) {
    char wire[512];
    snprintf(wire, sizeof(wire), "%s3\r\nabc\r\n0\r\nX-End: yes\r\n\r\n", chunked_header);
    struct sockaddr_in endpoint;
    pid_t child = start_backend(wire, &endpoint);
    struct http_request request = get_request();
    struct router_response response;
    start_tracking(-1);
    proxy_prepare_response(&endpoint, &request, NULL, &response);
    assert(response.status == 200 && response.chunked);
    struct proxy_stream *stream = response.body_provider.state;
    assert(live_allocations == 3); /* state + exact wire + Connection rows */
    assert(allocation_size(stream) == sizeof(*stream));
    assert(allocation_size(stream->client_header) == response.wire_header.length);
    assert(stream->connection_fields_length == sizeof(retained_connection) - 1);
    assert(allocation_size(stream->connection_fields) == sizeof(retained_connection) - 1);
    assert(!memcmp(stream->connection_fields, retained_connection, sizeof(retained_connection) - 1));
    assert(stream->trailer_buffer == NULL && response.wire_header.data == stream->client_header);
    assert(!memcmp(response.wire_header.data, "HTTP/1.1 200 OK\r\n", 17));
    struct http_slice chunk;
    assert(response.body_provider.next_func(stream, &chunk) == BODY_DATA);
    assert(chunk.length == 3 && !memcmp(chunk.data, "abc", 3));
    assert(stream->client_header == NULL && live_allocations == 2);
    assert(response.body_provider.next_func(stream, &chunk) == BODY_END);
    assert(stream->trailers.length == 12);
    assert(!memcmp(stream->trailers.data, "X-End: yes\r\n", 12));
    assert(allocation_size(stream->trailer_buffer) == stream->trailers.length);
    assert(live_allocations == 3);
    assert_cleanup_closes_backend(&response);
    stop_tracking();
    wait_backend(child);
}

static void test_zero_body_and_header_failure_cleanup(void) {
    static const char *wire[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 204 No Content\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"
    };
    for (size_t i = 0; i < 3; ++i) {
        struct sockaddr_in endpoint;
        pid_t child = start_backend(wire[i], &endpoint);
        struct http_request request = get_request();
        struct router_response response;
        start_tracking(-1);
        proxy_prepare_response(&endpoint, &request, NULL, &response);
        assert(response.status == (i == 1 ? 204 : 200));
        struct proxy_stream *stream = response.body_provider.state;
        assert(stream->client_header && !stream->connection_fields && !stream->trailer_buffer);
        assert(live_allocations == 2);
        /* No next call: zero body or consumer's header-send failure. */
        assert_cleanup_closes_backend(&response);
        stop_tracking();
        wait_backend(child);
    }
}

static void test_trailer_rejection_and_allocation_failure(void) {
    static const char *endings[] = {
        "0\r\nX-sEcOnD: forbidden\r\n\r\n", /* repeated Connection row, case insensitive */
        "0\r\nContent-Length: 0\r\n\r\n",
        "0\r\nX-End: yes\r\n\r\n", /* inject allocation failure after headers */
        "0\r\n\r\n" /* no trailer allocation */
    };
    for (size_t i = 0; i < 4; ++i) {
        char wire[512];
        snprintf(wire, sizeof(wire), "%s%s", chunked_header, endings[i]);
        struct sockaddr_in endpoint;
        pid_t child = start_backend(wire, &endpoint);
        struct http_request request = get_request();
        struct router_response response;
        start_tracking(-1);
        proxy_prepare_response(&endpoint, &request, NULL, &response);
        assert(response.status == 200);
        struct proxy_stream *stream = response.body_provider.state;
        if (i == 2) allocations_before_failure = 0;
        struct http_slice chunk;
        enum body_result result = response.body_provider.next_func(stream, &chunk);
        assert(result == (i == 3 ? BODY_END : BODY_ERROR));
        if (i < 2) assert(errno == EPROTO);
        if (i == 2) assert(errno == ENOMEM);
        assert(!stream->client_header && !stream->trailer_buffer);
        assert(stream->trailers.length == 0 && live_allocations == 2);
        assert_cleanup_closes_backend(&response);
        stop_tracking();
        wait_backend(child);
    }
}

static void test_trailer_storage_bounds(void) {
    for (size_t extra = 0; extra <= 2; extra += 2) {
        char wire[9000];
        size_t used = (size_t)snprintf(wire, sizeof(wire), "%s0\r\n", chunked_header);
        for (size_t row = 0; row < 2; ++row) {
            memcpy(wire + used, "X-A: ", 5);
            used += 5;
            size_t value_length = 4089 + (row == 1 ? extra : 0);
            memset(wire + used, 'a', value_length);
            used += value_length;
            memcpy(wire + used, "\r\n", 2);
            used += 2;
        }
        memcpy(wire + used, "\r\n", 3);
        struct sockaddr_in endpoint;
        pid_t child = start_backend(wire, &endpoint);
        struct http_request request = get_request();
        struct router_response response;
        start_tracking(-1);
        proxy_prepare_response(&endpoint, &request, NULL, &response);
        assert(response.status == 200);
        struct proxy_stream *stream = response.body_provider.state;
        struct http_slice chunk;
        enum body_result result = response.body_provider.next_func(stream, &chunk);
        if (extra == 0) {
            assert(result == BODY_END && stream->trailers.length == REQ_HEADER_MAX_BYTES);
            assert(allocation_size(stream->trailer_buffer) == REQ_HEADER_MAX_BYTES);
        } else {
            assert(result == BODY_ERROR && errno == EOVERFLOW);
            assert(!stream->trailer_buffer && stream->trailers.length == 0);
        }
        assert_cleanup_closes_backend(&response);
        stop_tracking();
        wait_backend(child);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_preparation_allocation_failures();
    test_header_and_trailer_lifetimes();
    test_zero_body_and_header_failure_cleanup();
    test_trailer_rejection_and_allocation_failure();
    test_trailer_storage_bounds();
    puts("[PASS] proxy buffer lifetimes, allocation failures and backend FD cleanup");
    return 0;
}