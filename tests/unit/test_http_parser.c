#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
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

static struct http_request route_query_input(const char *method, const char *path,
                                             const char *query) {
    struct http_request request = route_input(method, path);
    if (query) {
        request.query = (struct http_slice){(const unsigned char *)query, strlen(query)};
    }
    return request;
}

static void drain_provider(struct router_response *response, unsigned char *out,
                           size_t cap, size_t *out_len) {
    size_t total = 0;
    for (;;) {
        struct http_slice chunk = {0};
        enum body_result res = response->body_provider.next_func(response->body_provider.state, &chunk);
        if (res == BODY_END) break;
        assert(res == BODY_DATA);
        assert(chunk.data && chunk.length > 0);
        assert(total + chunk.length <= cap);
        memcpy(out + total, chunk.data, chunk.length);
        total += chunk.length;
    }
    *out_len = total;
    if (response->body_provider.cleanup_func) {
        response->body_provider.cleanup_func(response->body_provider.state);
    }
}

static void query_separation(void) {
    struct {
        const char *wire;
        const char *path;
        const char *query;
    } cases[] = {
        {"GET /greet HTTP/1.1\r\nHost: a\r\n\r\n", "/greet", ""},
        {"GET /greet? HTTP/1.1\r\nHost: a\r\n\r\n", "/greet", ""},
        {"GET /greet?name=Alice HTTP/1.1\r\nHost: a\r\n\r\n", "/greet", "name=Alice"},
        {"GET /greet?name=a?b&x=1 HTTP/1.1\r\nHost: a\r\n\r\n", "/greet", "name=a?b&x=1"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct http_request req;
        assert(http_parse_request((const unsigned char *)cases[i].wire,
                                  strlen(cases[i].wire), &req) == 0);
        assert(http_slice_equal(req.path, cases[i].path));
        assert(http_slice_equal(req.query, cases[i].query));
    }
}

static void dynamic_route_selection(void) {
    static const struct {
        const char *method, *path;
        int status;
        const char *allow;
    } cases[] = {
        {"GET", "/greet", 0, NULL},
        {"GET", "/greet/extra", 404, NULL},
        {"GET", "/%67reet", 404, NULL},
        {"POST", "/greet", 405, "GET"},
        {"DELETE", "/greet", 501, NULL},
        {"GET", "/cgi-bin/order", 0, NULL},
        {"GET", "/cgi-bin/", 0, NULL},
        {"GET", "/cgi-bin", 404, NULL},
        {"GET", "/%63gi-bin/order", 404, NULL},
        {"POST", "/cgi-bin/order", 405, "GET"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct http_request req = route_input(cases[i].method, cases[i].path);
        router_handler fn = NULL;
        const char *allow = NULL;
        assert(http_select_handler(&req, &fn, &allow) == cases[i].status);
        assert((fn != NULL) == (cases[i].status == 0));
        if (cases[i].allow) assert(allow && strcmp(allow, cases[i].allow) == 0);
        else assert(allow == NULL);
    }
}

static bool contains_bytes(const unsigned char *haystack, size_t hlen,
                           const unsigned char *needle, size_t nlen) {
    if (nlen == 0) return true;
    if (hlen < nlen) return false;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (memcmp(haystack + i, needle, nlen) == 0) return true;
    }
    return false;
}

static void dynamic_greet_responses(void) {
    static const struct {
        const char *query;
        int status;
        const char *expected_substr;
    } cases[] = {
        {NULL, 200, "어서 오세요, 손님님!"},
        {"", 200, "어서 오세요, 손님님!"},
        {"foo=1", 200, "어서 오세요, 손님님!"},
        {"name", 200, "어서 오세요, 손님님!"},
        {"name=", 200, "어서 오세요, 손님님!"},
        {"name=%20+", 200, "어서 오세요, 손님님!"},
        {"Name=Alice", 200, "어서 오세요, 손님님!"},
        {"name=+Alice+Kim+", 200, "어서 오세요, Alice Kim님!"},
        {"%6E%61%6D%65=Alice", 200, "어서 오세요, Alice님!"},
        {"&&foo=1&name=A%26B%3DC&&", 200, "어서 오세요, A&amp;B=C님!"},
        {"name=a=b", 200, "어서 오세요, a=b님!"},
        {"name=Alice&name=Bob", 200, "어서 오세요, Alice님!"},
        {"name&name=Alice", 200, "어서 오세요, 손님님!"},
        {"name=%3Cscript%3E%26%22%27", 200, "어서 오세요, &lt;script&gt;&amp;&quot;&#39;님!"},
        {"name=%ED%99%8D%EA%B8%B8%EB%8F%99", 200, "어서 오세요, 홍길동님!"},
        {"name=Alice%00", 400, NULL},
        {"other=%00&name=Alice", 400, NULL},
        {"na%00me=Alice", 400, NULL},
        {"name=bad%2", 400, NULL},
        {"name=%ZZ", 400, NULL},
        {"name=a%0Ab", 400, NULL},
        {"name=%09Alice", 400, NULL},
        {"name=a%7Fb", 400, NULL},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct http_request req = route_query_input("GET", "/greet", cases[i].query);
        static const unsigned char ignored_body[] = "hello";
        req.body = (struct http_slice){ignored_body, sizeof(ignored_body) - 1};

        router_handler fn = NULL;
        const char *allow = NULL;
        assert(http_select_handler(&req, &fn, &allow) == 0);

        struct router_response resp;
        fn(&req, &resp);
        assert(resp.status == cases[i].status);
        assert(!resp.chunked);

        unsigned char buf[4096];
        size_t len = 0;
        drain_provider(&resp, buf, sizeof(buf), &len);
        assert(len == resp.length);

        if (cases[i].status == 200) {
            assert(strcmp(resp.content_type, "text/html; charset=utf-8") == 0);
            assert(contains_bytes(buf, len,
                                  (const unsigned char *)cases[i].expected_substr,
                                  strlen(cases[i].expected_substr)));
            assert(!contains_bytes(buf, len, (const unsigned char *)"<script>", 8));
        } else {
            assert(strcmp(resp.content_type, "text/plain") == 0);
        }
    }

    /* Invalid UTF-8 bytes are copied raw without validation. */
    struct http_request invalid_utf8 = route_query_input("GET", "/greet", "name=%FF%FE");
    router_handler fn = NULL;
    const char *allow = NULL;
    assert(http_select_handler(&invalid_utf8, &fn, &allow) == 0);
    struct router_response resp;
    fn(&invalid_utf8, &resp);
    assert(resp.status == 200);
    unsigned char buf[4096];
    size_t len = 0;
    drain_provider(&resp, buf, sizeof(buf), &len);
    static const unsigned char raw_seq[] = {0xFF, 0xFE};
    assert(contains_bytes(buf, len, raw_seq, sizeof(raw_seq)));

    /* Long name expanding by 5x under HTML escaping (within REQ_HEADER_MAX_BYTES). */
    char long_query[5 + 900 + 1];
    memcpy(long_query, "name=", 5);
    memset(long_query + 5, '\'', 900);
    long_query[sizeof(long_query) - 1] = '\0';
    struct http_request long_req = route_query_input("GET", "/greet", long_query);
    assert(http_select_handler(&long_req, &fn, &allow) == 0);
    fn(&long_req, &resp);
    assert(resp.status == 200 && !resp.chunked);
    unsigned char *long_buf = malloc(resp.length + 1);
    assert(long_buf != NULL);
    size_t long_len = 0;
    drain_provider(&resp, long_buf, resp.length + 1, &long_len);
    assert(long_len == resp.length && long_len > 900 * 5);
    assert(contains_bytes(long_buf, long_len, (const unsigned char *)"&#39;&#39;&#39;", 15));
    free(long_buf);
}

static void make_script(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(fd >= 0);
    size_t len = strlen(content);
    assert(write(fd, content, len) == (ssize_t)len);
    assert(close(fd) == 0);
}

static void cgi_responses_and_boundaries(void) {
    char temp[] = "/tmp/iter3-cgi-XXXXXX";
    int old_cwd = open(".", O_RDONLY);
    assert(old_cwd >= 0 && mkdtemp(temp) != NULL);
    assert(chdir(temp) == 0);

    /* Without cgi-bin directory, CGI requests return 404. */
    struct http_request req = route_query_input("GET", "/cgi-bin/ok", NULL);
    router_handler fn = NULL;
    const char *allow = NULL;
    assert(http_select_handler(&req, &fn, &allow) == 0);
    struct router_response resp;
    fn(&req, &resp);
    assert(resp.status == 404);
    if (resp.body_provider.cleanup_func) resp.body_provider.cleanup_func(resp.body_provider.state);

    assert(mkdir("cgi-bin", 0700) == 0);
    assert(mkdir("cgi-bin/sub", 0700) == 0);
    assert(mkdir("cgi-bin-other", 0700) == 0);
    make_script("outside.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\nout'\n", 0700);
    make_script("cgi-bin-other/sib.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\nsib'\n", 0700);
    make_script("cgi-bin/nonexec.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\nno'\n", 0600);
    make_script("cgi-bin/sub/nested.sh", "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\nsub'\n", 0700);
    assert(symlink("../outside.sh", "cgi-bin/outlink.sh") == 0);
    assert(symlink("../cgi-bin-other/sib.sh", "cgi-bin/siblink.sh") == 0);
    assert(mkfifo("cgi-bin/fifo.sh", 0600) == 0);

    /* Encoded path -> 400 */
    req = route_query_input("GET", "/cgi-bin/%6fk", NULL);
    assert(http_select_handler(&req, &fn, &allow) == 0);
    fn(&req, &resp);
    assert(resp.status == 400);
    if (resp.body_provider.cleanup_func) resp.body_provider.cleanup_func(resp.body_provider.state);

    /* Boundary / permission / non-regular / nested -> 404 */
    const char *reject404[] = {
        "/cgi-bin/", "/cgi-bin/missing", "/cgi-bin/sub", "/cgi-bin/sub/nested.sh",
        "/cgi-bin/ok/extra", "/cgi-bin/../outside.sh", "/cgi-bin/outlink.sh",
        "/cgi-bin/siblink.sh", "/cgi-bin/fifo.sh", "/cgi-bin/nonexec.sh"
    };
    for (size_t i = 0; i < sizeof(reject404) / sizeof(reject404[0]); ++i) {
        req = route_query_input("GET", reject404[i], NULL);
        assert(http_select_handler(&req, &fn, &allow) == 0);
        fn(&req, &resp);
        assert(resp.status == 404);
        if (resp.body_provider.cleanup_func) resp.body_provider.cleanup_func(resp.body_provider.state);
    }

    /* Normal CGI with LF headers, environment isolation, and chunked flag */
    assert(setenv("ITER3_PARENT_SECRET", "do-not-leak", 1) == 0);
    make_script("cgi-bin/ok.sh",
                "#!/bin/sh\n"
                "printf 'Content-Type: text/plain\\nContent-Length: 999\\n\\n'\n"
                "printf 'q=%s|m=%s|s=%s|sec=%s' \"$QUERY_STRING\" \"$REQUEST_METHOD\" \"$SCRIPT_NAME\" \"$ITER3_PARENT_SECRET\"\n",
                0700);

    req = route_query_input("GET", "/cgi-bin/ok.sh", "a=1&b=%20");
    assert(http_select_handler(&req, &fn, &allow) == 0);
    fn(&req, &resp);
    assert(resp.status == 200 && resp.chunked);
    assert(strcmp(resp.content_type, "text/plain") == 0);

    unsigned char cgi_out[4096];
    size_t cgi_len = 0;
    drain_provider(&resp, cgi_out, sizeof(cgi_out), &cgi_len);
    assert(cgi_len == strlen("q=a=1&b=%20|m=GET|s=/cgi-bin/ok.sh|sec="));
    assert(memcmp(cgi_out, "q=a=1&b=%20|m=GET|s=/cgi-bin/ok.sh|sec=", cgi_len) == 0);

    /* CRLF headers + custom Status: 400 */
    make_script("cgi-bin/status400.sh",
                "#!/bin/sh\n"
                "printf 'Status: 400 Bad Request\\r\\nContent-Type: text/html; charset=utf-8\\r\\n\\r\\nbad'\n",
                0700);
    req = route_query_input("GET", "/cgi-bin/status400.sh", NULL);
    fn(&req, &resp);
    assert(resp.status == 400 && resp.chunked);
    assert(strcmp(resp.content_type, "text/html; charset=utf-8") == 0);
    drain_provider(&resp, cgi_out, sizeof(cgi_out), &cgi_len);
    assert(cgi_len == 3 && memcmp(cgi_out, "bad", 3) == 0);

    /* 502 failures: missing Content-Type, empty stdout, bad Status, bad shebang, oversized header */
    make_script("cgi-bin/notype.sh", "#!/bin/sh\nprintf 'Status: 200 OK\\n\\nhi'\n", 0700);
    make_script("cgi-bin/empty.sh", "#!/bin/sh\nexit 0\n", 0700);
    make_script("cgi-bin/badstatus.sh", "#!/bin/sh\nprintf 'Status: 999 Weird\\nContent-Type: text/plain\\n\\nhi'\n", 0700);
    make_script("cgi-bin/badexec.sh", "#!/nonexistent/interp\n", 0700);
    make_script("cgi-bin/bighdr.sh",
                "#!/bin/sh\n"
                "printf 'Content-Type: text/plain\\nX-Big: '\n"
                "dd if=/dev/zero bs=1 count=8300 2>/dev/null | tr '\\0' 'a'\n"
                "printf '\\n\\nhi'\n",
                0700);
    const char *bad_cgis[] = {
        "/cgi-bin/notype.sh", "/cgi-bin/empty.sh", "/cgi-bin/badstatus.sh",
        "/cgi-bin/badexec.sh", "/cgi-bin/bighdr.sh"
    };
    for (size_t i = 0; i < sizeof(bad_cgis) / sizeof(bad_cgis[0]); ++i) {
        req = route_query_input("GET", bad_cgis[i], NULL);
        fn(&req, &resp);
        assert(resp.status == 502 && !resp.chunked);
        if (resp.body_provider.cleanup_func) resp.body_provider.cleanup_func(resp.body_provider.state);
    }

    /* Mid-body crash -> BODY_ERROR and child reaped */
    make_script("cgi-bin/midcrash.sh",
                "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\npart'\nexit 2\n", 0700);
    req = route_query_input("GET", "/cgi-bin/midcrash.sh", NULL);
    fn(&req, &resp);
    assert(resp.status == 200 && resp.chunked);
    struct http_slice chunk = {0};
    assert(resp.body_provider.next_func(resp.body_provider.state, &chunk) == BODY_DATA);
    assert(chunk.length == 4 && memcmp(chunk.data, "part", 4) == 0);
    assert(resp.body_provider.next_func(resp.body_provider.state, &chunk) == BODY_ERROR);
    resp.body_provider.cleanup_func(resp.body_provider.state);

    /* Cleanup before EOF kills & reaps sleeping child (no zombie left) */
    make_script("cgi-bin/sleepy.sh",
                "#!/bin/sh\nprintf 'Content-Type: text/plain\\n\\nstart'\nsleep 30\n", 0700);
    req = route_query_input("GET", "/cgi-bin/sleepy.sh", NULL);
    fn(&req, &resp);
    assert(resp.status == 200 && resp.chunked);
    resp.body_provider.cleanup_func(resp.body_provider.state);
    int wstatus = 0;
    assert(waitpid(-1, &wstatus, WNOHANG) == -1 && errno == ECHILD);

    unsetenv("ITER3_PARENT_SECRET");
    assert(unlink("cgi-bin/sleepy.sh") == 0);
    assert(unlink("cgi-bin/midcrash.sh") == 0);
    assert(unlink("cgi-bin/bighdr.sh") == 0);
    assert(unlink("cgi-bin/badexec.sh") == 0);
    assert(unlink("cgi-bin/badstatus.sh") == 0);
    assert(unlink("cgi-bin/empty.sh") == 0);
    assert(unlink("cgi-bin/notype.sh") == 0);
    assert(unlink("cgi-bin/status400.sh") == 0);
    assert(unlink("cgi-bin/ok.sh") == 0);
    assert(unlink("cgi-bin/fifo.sh") == 0);
    assert(unlink("cgi-bin/siblink.sh") == 0);
    assert(unlink("cgi-bin/outlink.sh") == 0);
    assert(unlink("cgi-bin/sub/nested.sh") == 0);
    assert(unlink("cgi-bin/nonexec.sh") == 0);
    assert(unlink("cgi-bin-other/sib.sh") == 0);
    assert(unlink("outside.sh") == 0);
    assert(rmdir("cgi-bin-other") == 0);
    assert(rmdir("cgi-bin/sub") == 0);
    assert(rmdir("cgi-bin") == 0);
    assert(fchdir(old_cwd) == 0);
    assert(close(old_cwd) == 0);
    assert(rmdir(temp) == 0);
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
    query_separation();
    dynamic_route_selection();
    dynamic_greet_responses();
    cgi_responses_and_boundaries();
    puts("[PASS] test_http_parser");
    return 0;
}
