#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../src/router.h"

static void valid_request(void) {
    unsigned char wire[] = "POST /echo?x=%20 HTTP/1.1\r\nhOsT:\tlocalhost:8080 \t\r\n"
                           "Content-Length: 5\r\nConnection: Keep-Alive, close\r\n"
                           "Expect: 100-continue\r\nX-Unused: value\r\n\r\n";
    unsigned char original[sizeof(wire)];
    memcpy(original, wire, sizeof(wire));
    struct http_request request;
    assert(http_parse_request(wire, sizeof(wire) - 1, &request) == 0);
    assert(memcmp(wire, original, sizeof(wire)) == 0);
    assert(http_slice_equal(request.method, "POST"));
    assert(http_slice_equal(request.version, "HTTP/1.1"));
    assert(http_slice_equal(request.path, "/echo"));
    assert(http_slice_equal(request.host, "localhost:8080"));
    assert(request.method.data == wire);
    assert(request.target.data >= wire && request.target.data < wire + sizeof(wire));
    assert(request.content_length == 5 && request.has_content_length);
    assert(request.expect_continue && request.connection_close && request.connection_keep_alive);
    assert(request.body.length == 0 && request.body.data == NULL);
    router_handler route_handler_func;
    const char *allow;
    assert(http_select_handler(&request, &route_handler_func, &allow) == 0 && route_handler_func);
}

static void basic_statuses(void) {
    static const struct { const char *wire; int status; } cases[] = {
        {"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: localhost\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: [::1]:8080\r\nContent-Length: 0\r\n\r\n", 0},
        {"GET /%68ello HTTP/1.1\r\nHost: localhost\r\n\r\n", 0},
        {"GET /hello HTTP/1.0\r\nHost: localhost\r\n\r\n", 505},
        {"GET http://localhost/hello HTTP/1.1\r\nHost: localhost\r\n\r\n", 501},
        {"GET / HTTP/1.1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a\r\nHost: a\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a b\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: []\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: a\r\n Folded: b\r\n\r\n", 400},
        {"GET /bad%2 HTTP/1.1\r\nHost: a\r\n\r\n", 400},
        {"GET /bad#fragment HTTP/1.1\r\nHost: a\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: +1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 18446744073709551616\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65535\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65536\r\n\r\n", 0},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 65537\r\n\r\n", 413},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked; x=1\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, chunked\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
        {"POST /echo HTTP/1.1\r\nHost: a\r\nExpect: other\r\n\r\n", 417},
        {"GET / HTTP/1.1\r\nHost: a\r\nConnection: close,\r\n\r\n", 400}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct http_request request;
        int status = http_parse_request((const unsigned char *)cases[i].wire,
                                         strlen(cases[i].wire), &request);
        if (status != cases[i].status) fprintf(stderr, "parser case %zu: %d != %d\n", i, status, cases[i].status);
        assert(status == cases[i].status);
    }
    static const unsigned char nul[] = "GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n";
    struct http_request request;
    assert(http_parse_request(nul, sizeof(nul) - 1, &request) == 400);
}

static void routes(void) {
    static const struct { const char *method; const char *target; int status; const char *allow; } cases[] = {
        {"GET", "/", 0, NULL}, {"GET", "/hello?x=1", 0, NULL},
        {"POST", "/echo", 0, NULL}, {"GET", "/echo", 405, "POST"},
        {"POST", "/hello", 405, "GET"}, {"GET", "/missing", 404, NULL},
        {"GET", "/%68ello", 404, NULL}, {"GET", "/a/../hello", 404, NULL},
        {"DELETE", "/hello", 501, NULL}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char wire[256];
        int length = snprintf(wire, sizeof(wire), "%s %s HTTP/1.1\r\nHost: a\r\n\r\n", cases[i].method, cases[i].target);
        assert(length > 0 && (size_t)length < sizeof(wire));
        struct http_request request;
        assert(http_parse_request((const unsigned char *)wire, (size_t)length, &request) == 0);
        router_handler route_handler_func;
        const char *allow;
        assert(http_select_handler(&request, &route_handler_func, &allow) == cases[i].status);
        assert((route_handler_func != NULL) == (cases[i].status == 0));
        if (cases[i].allow) assert(allow && strcmp(allow, cases[i].allow) == 0);
        else assert(allow == NULL);
    }
}

static void response_selection(void) {
    unsigned char bytes[] = {'a', 0, 'b', '\r', '\n'};
    struct http_request request = {0};
    request.body = (struct http_slice){bytes, sizeof(bytes)};
    request.method = (struct http_slice){(const unsigned char *)"POST", 4};
    request.path = (struct http_slice){(const unsigned char *)"/echo", 5};
    router_handler route_handler_func;
    const char *allow;
    struct router_response response;
    assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
    route_handler_func(&request, &response);
    assert(response.status == 200 && response.allow == NULL);
    assert(strcmp(response.content_type, "application/octet-stream") == 0);
    assert(response.body == bytes && response.length == sizeof(bytes));

    request.body = (struct http_slice){NULL, 0};
    route_handler_func(&request, &response);
    assert(response.body == NULL && response.length == 0);
    request.method = (struct http_slice){(const unsigned char *)"GET", 3};
    request.path = (struct http_slice){(const unsigned char *)"/hello", 6};
    assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
    route_handler_func(&request, &response);
    assert(strcmp(response.content_type, "text/plain") == 0);
    assert(response.length == 5 && memcmp(response.body, "Hello", 5) == 0);
    router_prepare_error(&response, 405, "GET");
    assert(response.status == 405 && strcmp(response.allow, "GET") == 0);
    assert(response.length == strlen("Method Not Allowed"));
    assert(memcmp(response.body, "Method Not Allowed", response.length) == 0);
}

static struct http_request route_input(const char *method, const char *path) {
    struct http_request request = {0};
    request.method = (struct http_slice){(const unsigned char *)method, strlen(method)};
    request.path = (struct http_slice){(const unsigned char *)path, strlen(path)};
    return request;
}

static void static_route_selection(void) {
    static const struct {
        const char *method, *path;
        int status;
        const char *allow;
    } cases[] = {
        {"GET", "/cafe", 0, NULL},
        {"GET", "/menu", 0, NULL},
        {"GET", "/assets/a.css", 0, NULL},
        {"GET", "/assets/", 0, NULL},
        {"GET", "/assets", 404, NULL},
        {"GET", "/assets-old/a.css", 404, NULL},
        {"GET", "/cafe-more", 404, NULL},
        {"GET", "/%61ssets/a.css", 404, NULL},
        {"POST", "/assets/a.css", 405, "GET"},
        {"POST", "/cafe", 405, "GET"},
        {"DELETE", "/assets/a.css", 501, NULL}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct http_request request = route_input(cases[i].method, cases[i].path);
        router_handler route_handler_func = NULL;
        const char *allow = NULL;
        int status = http_select_handler(&request, &route_handler_func, &allow);
        assert(status == cases[i].status);
        if (cases[i].allow) assert(allow && strcmp(allow, cases[i].allow) == 0);
        else assert(allow == NULL);
        assert((route_handler_func != NULL) == (status == 0));
        if (cases[i].allow) assert(allow && strcmp(allow, cases[i].allow) == 0);
        else assert(allow == NULL);
    }
}

/* These are test fixture locations, not a new server configuration contract.
 * Override at compile time if the selected implementation's root changes. */
#ifndef TEST_STATIC_ROOT
#define TEST_STATIC_ROOT "public"
#endif

static void make_file(const char *path, const unsigned char *bytes, size_t length) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, bytes + offset, length - offset);
        assert(n > 0);
        offset += (size_t)n;
    }
    assert(close(fd) == 0);
}

static void static_file_responses(void) {
    char temp[] = "/tmp/iter3-unit-XXXXXX";
    int old_cwd = open(".", O_RDONLY);
    assert(old_cwd >= 0 && mkdtemp(temp) != NULL);
    assert(chdir(temp) == 0);
    assert(mkdir(TEST_STATIC_ROOT, 0700) == 0);
    assert(mkdir(TEST_STATIC_ROOT "/assets", 0700) == 0);
    assert(mkdir(TEST_STATIC_ROOT "/assets/with.dot", 0700) == 0);

    static const unsigned char binary[] = {0x00, 0xff, 'x', '\r', '\n'};
    static const struct { const char *name, *type; } files[] = {
        {"a.html", "text/html"}, {"a.CSS", "text/css"},
        {"a.Js", "text/javascript"}, {"a.PNG", "image/png"},
        {"a.jpg", "image/jpeg"}, {"a.JpEg", "image/jpeg"}
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        char file[256], target[256];
        assert(snprintf(file, sizeof(file), TEST_STATIC_ROOT "/assets/%s", files[i].name) > 0);
        assert(snprintf(target, sizeof(target), "/assets/%s", files[i].name) > 0);
        make_file(file, binary, sizeof(binary));
        struct http_request request = route_input("GET", target);
        router_handler route_handler_func = NULL;
        const char *allow = NULL;
        assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
        struct router_response response;
        route_handler_func(&request, &response);
        assert(response.status == 200);
        assert(strcmp(response.content_type, files[i].type) == 0);
        assert(response.length == sizeof(binary));
        assert(response.body_provider.next_func != NULL);
        size_t offset = 0;
        while (offset < sizeof(binary)) {
            struct http_slice part = {0};
            assert(response.body_provider.next_func(response.body_provider.state, &part) == BODY_DATA);
            assert(part.data && part.length > 0 && part.length <= sizeof(binary) - offset);
            assert(memcmp(part.data, binary + offset, part.length) == 0);
            offset += part.length;
        }
        if (response.body_provider.cleanup_func) response.body_provider.cleanup_func(response.body_provider.state);
        assert(unlink(file) == 0);
    }

    make_file(TEST_STATIC_ROOT "/assets/empty.css", binary, 0);
    make_file(TEST_STATIC_ROOT "/assets/with.dot/noext", binary, sizeof(binary));
    make_file(TEST_STATIC_ROOT "/assets/unknown.cssx", binary, sizeof(binary));
    make_file(TEST_STATIC_ROOT "/outside.css", binary, sizeof(binary));
    make_file(TEST_STATIC_ROOT "/assets/inside.css", binary, sizeof(binary));
    assert(symlink("inside.css", TEST_STATIC_ROOT "/assets/link.css") == 0);
    assert(symlink("../outside.css", TEST_STATIC_ROOT "/assets/outside.css") == 0);
    assert(mkfifo(TEST_STATIC_ROOT "/assets/fifo.css", 0600) == 0);
    assert(mkdir(TEST_STATIC_ROOT "/assets/dir.css", 0700) == 0);
    static const struct { const char *target; int status; size_t length; } boundaries[] = {
        {"/assets/empty.css", 200, 0},
        {"/assets/link.css", 200, sizeof(binary)},
        {"/assets/outside.css", 404, 0},
        {"/assets/../outside.css", 404, 0},
        {"/assets/fifo.css", 404, 0},
        {"/assets/dir.css", 404, 0},
        {"/assets/with.dot/noext", 404, 0},
        {"/assets/unknown.cssx", 404, 0},
        {"/assets/missing.css", 404, 0},
        {"/assets/a%2Ecss", 400, 0}
    };
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        struct http_request request = route_input("GET", boundaries[i].target);
        router_handler route_handler_func = NULL;
        const char *allow = NULL;
        assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
        struct router_response response;
        /* A regression that blocks while opening a FIFO must fail, not hang. */
        alarm(5);
        route_handler_func(&request, &response);
        alarm(0);
        assert(response.status == boundaries[i].status);
        if (response.status == 200) {
            assert(response.length == boundaries[i].length);
            assert(strcmp(response.content_type, "text/css") == 0);
            size_t offset = 0;
            while (offset < response.length) {
                struct http_slice part = {0};
                assert(response.body_provider.next_func(response.body_provider.state, &part) == BODY_DATA);
                assert(part.data && part.length > 0 && part.length <= response.length - offset);
                assert(memcmp(part.data, binary + offset, part.length) == 0);
                offset += part.length;
            }
        }
        if (response.body_provider.cleanup_func) response.body_provider.cleanup_func(response.body_provider.state);
    }
    /* A low FD limit makes leaked file descriptors observable across requests. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit limit;
        assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
        if (limit.rlim_cur > 32) {
            limit.rlim_cur = 32;
            assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
        }
        for (int i = 0; i < 100; ++i) {
            struct http_request request = route_input("GET", "/assets/inside.css");
            router_handler route_handler_func = NULL;
            const char *allow = NULL;
            struct router_response response;
            assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
            route_handler_func(&request, &response);
            assert(response.status == 200);
            if (response.body_provider.cleanup_func)
                response.body_provider.cleanup_func(response.body_provider.state);
            request = route_input("GET", "/assets/dir.css");
            assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
            route_handler_func(&request, &response);
            assert(response.status == 404);
            if (response.body_provider.cleanup_func)
                response.body_provider.cleanup_func(response.body_provider.state);
        }
        _exit(0);
    }
    int child_status;
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    assert(unlink(TEST_STATIC_ROOT "/assets/empty.css") == 0);
    assert(unlink(TEST_STATIC_ROOT "/assets/with.dot/noext") == 0);
    assert(unlink(TEST_STATIC_ROOT "/assets/unknown.cssx") == 0);
    assert(unlink(TEST_STATIC_ROOT "/assets/link.css") == 0);
    assert(unlink(TEST_STATIC_ROOT "/assets/outside.css") == 0);
    assert(unlink(TEST_STATIC_ROOT "/assets/inside.css") == 0);
    assert(unlink(TEST_STATIC_ROOT "/assets/fifo.css") == 0);
    assert(unlink(TEST_STATIC_ROOT "/outside.css") == 0);
    assert(rmdir(TEST_STATIC_ROOT "/assets/dir.css") == 0);
    assert(rmdir(TEST_STATIC_ROOT "/assets/with.dot") == 0);
    assert(rmdir(TEST_STATIC_ROOT "/assets") == 0);
    assert(rmdir(TEST_STATIC_ROOT) == 0);
    assert(fchdir(old_cwd) == 0);
    assert(close(old_cwd) == 0);
    assert(rmdir(temp) == 0);
}

static void file_provider_chunks_and_early_eof(void) {
    char temp[] = "/tmp/iter3-provider-XXXXXX";
    int old_cwd = open(".", O_RDONLY);
    assert(old_cwd >= 0 && mkdtemp(temp) != NULL);
    assert(chdir(temp) == 0);
    assert(mkdir(TEST_STATIC_ROOT, 0700) == 0);
    assert(mkdir(TEST_STATIC_ROOT "/assets", 0700) == 0);
    unsigned char bytes[65537];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)i;
    make_file(TEST_STATIC_ROOT "/assets/chunks.js", bytes, sizeof(bytes));
    struct http_request request = route_input("GET", "/assets/chunks.js");
    router_handler route_handler_func = NULL;
    const char *allow = NULL;
    assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
    struct router_response response;
    route_handler_func(&request, &response);
    assert(response.status == 200 && response.length == sizeof(bytes));
    assert(response.body_provider.next_func);
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        struct http_slice part = {0};
        assert(response.body_provider.next_func(response.body_provider.state, &part) == BODY_DATA);
        assert(part.data && part.length && part.length <= sizeof(bytes) - offset);
        assert(memcmp(part.data, bytes + offset, part.length) == 0);
        offset += part.length;
    }
    struct http_slice part = {0};
    if (response.body_provider.cleanup_func) response.body_provider.cleanup_func(response.body_provider.state);

    /* File shrinks after its length was prepared: provider reports early EOF.
     * This tests the real provider, not the server's response-abort policy. */
    route_handler_func(&request, &response);
    assert(response.status == 200 && response.length == sizeof(bytes));
    assert(truncate(TEST_STATIC_ROOT "/assets/chunks.js", 0) == 0);
    assert(response.body_provider.next_func(response.body_provider.state, &part) == BODY_END);
    response.body_provider.cleanup_func(response.body_provider.state);
    assert(unlink(TEST_STATIC_ROOT "/assets/chunks.js") == 0);
    assert(rmdir(TEST_STATIC_ROOT "/assets") == 0);
    assert(rmdir(TEST_STATIC_ROOT) == 0);
    assert(fchdir(old_cwd) == 0);
    assert(close(old_cwd) == 0);
    assert(rmdir(temp) == 0);
}

static void memory_provider_contract(void) {
    static const unsigned char bytes[] = {'a', 0, 'b'};
    struct http_request request = route_input("POST", "/echo");
    request.body = (struct http_slice){bytes, sizeof(bytes)};
    router_handler route_handler_func = NULL;
    const char *allow = NULL;
    assert(http_select_handler(&request, &route_handler_func, &allow) == 0);
    struct router_response response;
    route_handler_func(&request, &response);
    assert(response.length == sizeof(bytes) && response.body_provider.next_func);
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        struct http_slice part = {0};
        assert(response.body_provider.next_func(response.body_provider.state, &part) == BODY_DATA);
        assert(part.data && part.length > 0 && part.length <= sizeof(bytes) - offset);
        assert(memcmp(part.data, bytes + offset, part.length) == 0);
        offset += part.length;
    }
    if (response.body_provider.cleanup_func) response.body_provider.cleanup_func(response.body_provider.state);

    request.body = (struct http_slice){NULL, 0};
    route_handler_func(&request, &response);
    assert(response.length == 0 && response.body_provider.next_func);
    if (response.body_provider.cleanup_func) response.body_provider.cleanup_func(response.body_provider.state);
}

int main(void) {
    valid_request();
    basic_statuses();
    routes();
    response_selection();
    static_route_selection();
    static_file_responses();
    file_provider_chunks_and_early_eof();
    memory_provider_contract();
    puts("[PASS] test_http_parser");
    return 0;
}
