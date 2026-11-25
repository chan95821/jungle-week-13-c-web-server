#define _POSIX_C_SOURCE 200809L
#include "../../src/proxy.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct http_slice slice(const char *text) {
    return (struct http_slice){(const unsigned char *)text, strlen(text)};
}

static void write_config(const char *path, const void *data, size_t length) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void test_config(const char *path) {
    static const char valid[] =
        "# test-only endpoints\n\nquery-b 127.0.0.1:8003\r\n"
        "submission 127.0.0.1:8001 # submit\nquery-a 127.0.0.1:8002";
    struct proxy_config config = {0};
    write_config(path, valid, sizeof(valid) - 1);
    assert(proxy_load_config(path, &config) == 0);
    assert(config.submission_backend.sin_family == AF_INET);
    assert(ntohs(config.submission_backend.sin_port) == 8001);
    assert(ntohs(config.query_backends[0].sin_port) == 8002);
    assert(ntohs(config.query_backends[1].sin_port) == 8003);
    assert(config.query_backends[0].sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    static const char *invalid[] = {
        "",
        "submission 127.0.0.1:8001\nquery-a 127.0.0.1:8002",
        "unknown 127.0.0.1:8001",
        "submission 127.0.0.1:8001\nsubmission 127.0.0.1:8001",
        "submission localhost:8001",
        "submission [::1]:8001",
        "submission 256.1.1.1:8001",
        "submission 127.0.0.1:0",
        "submission 127.0.0.1:65536",
        "submission 127.0.0.1:8001x",
        "submission 127.0.0.1:8001 extra",
        "submission 127.0.0.1",
        "submission :8001",
        "submission 127.0.0.1:+8001"
    };
    struct proxy_config original;
    memcpy(&original, &config, sizeof(config));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        write_config(path, invalid[i], strlen(invalid[i]));
        assert(proxy_load_config(path, &config) == -1);
        assert(errno == EINVAL);
        assert(memcmp(&config, &original, sizeof(config)) == 0);
    }
    static const char embedded_nul[] = "submission 127.0.0.1:8001\0ignored\n";
    write_config(path, embedded_nul, sizeof(embedded_nul) - 1);
    assert(proxy_load_config(path, &config) == -1);
    assert(memcmp(&config, &original, sizeof(config)) == 0);
    assert(proxy_load_config(NULL, &config) == -1);
}

static void test_route_table(void) {
    static const struct {
        const char *path, *method;
        enum proxy_route destination;
    } rules[] = {
        {"/", "GET", PROXY_ROUTE_LOCAL}, {"/hello", "GET", PROXY_ROUTE_LOCAL},
        {"/echo", "POST", PROXY_ROUTE_LOCAL}, {"/cafe", "GET", PROXY_ROUTE_LOCAL},
        {"/menu", "GET", PROXY_ROUTE_LOCAL}, {"/assets/a.css", "GET", PROXY_ROUTE_LOCAL},
        {"/assets/", "GET", PROXY_ROUTE_LOCAL}, {"/greet", "GET", PROXY_ROUTE_LOCAL},
        {"/cgi-bin/order", "GET", PROXY_ROUTE_LOCAL},
        {"/cgi-bin/", "GET", PROXY_ROUTE_LOCAL},
        {"/api/submissions", "POST", PROXY_ROUTE_SUBMISSIONS},
        {"/api/submission-status", "GET", PROXY_ROUTE_QUERY},
        {"/api/hello", "GET", PROXY_ROUTE_QUERY}
    };
    static const char *methods[] = {"GET", "POST", "HEAD", "OPTIONS", "get"};
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
        for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); ++m) {
            struct http_request request = {
                .method = slice(methods[m]), .path = slice(rules[i].path),
                .target = slice("/unchanged?name=%20"),
                .headers = slice("Connection: Host\r\n")
            };
            struct request_route selected;
            const char *allow;
            bool implemented = m < 2;
            int expected = !implemented ? 501 :
                strcmp(methods[m], rules[i].method) == 0 ? 0 : 405;
            assert(http_select_route(&request, &selected, &allow) == expected);
            if (!expected) {
                assert(selected.proxy_route == rules[i].destination);
                assert((selected.local_handler != NULL) == (rules[i].destination == PROXY_ROUTE_LOCAL));
                assert(allow == NULL);
            } else {
                assert(selected.local_handler == NULL && selected.proxy_route == PROXY_ROUTE_LOCAL);
                assert(expected == 405 ? strcmp(allow, rules[i].method) == 0 : allow == NULL);
            }
            assert(http_slice_equal(request.target, "/unchanged?name=%20"));
            assert(http_slice_equal(request.headers, "Connection: Host\r\n"));
        }
    }
    static const char *unknown[] = {
        "/api/hello/", "/api/hello-extra", "/api/submissions-extra", "/api/unknown",
        "/assets", "/assets-extra/a.css", "/cgi-bin", "/cgi-bin-extra/order", "/hello/", "/missing"
    };
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        for (size_t m = 0; m < 3; ++m) {
            struct http_request request = {.method = slice(methods[m]), .path = slice(unknown[i])};
            struct request_route selected;
            const char *allow;
            assert(http_select_route(&request, &selected, &allow) == (m < 2 ? 404 : 501));
            assert(!selected.local_handler && selected.proxy_route == PROXY_ROUTE_LOCAL && !allow);
        }
    }
}

static void test_routes_and_selection(void) {
    struct proxy_config config = {
        .submission_backend = {.sin_port = htons(8001)},
        .query_backends = {{.sin_port = htons(8002)}, {.sin_port = htons(8003)}}
    };
    struct proxy_selection_state selection, independent_selection;
    assert(proxy_selection_init(&selection) == 0);
    assert(proxy_selection_init(&independent_selection) == 0);

    struct http_request request = {
        .method = slice("GET"), .path = slice("/api/submission-status"),
        .target = slice("/api/submission-status?id=7")
    };
    struct http_slice original_target = request.target;
    struct request_route selected;
    const char *allow;
    assert(http_select_route(&request, &selected, &allow) == 0);
    assert(selected.proxy_route == PROXY_ROUTE_QUERY && !selected.local_handler && !allow);
    assert(selection.next_query_backend == 0); /* Matching does not select/advance. */
    assert(proxy_select_backend(&config, &selection, selected.proxy_route) == &config.query_backends[0]);
    request.method = slice("POST"); request.path = slice("/api/submissions");
    assert(http_select_route(&request, &selected, &allow) == 0);
    assert(proxy_select_backend(&config, &selection, selected.proxy_route) == &config.submission_backend);
    assert(selection.next_query_backend == 1);
    request.method = slice("GET"); request.path = slice("/api/hello");
    assert(http_select_route(&request, &selected, &allow) == 0);
    assert(proxy_select_backend(&config, &selection, selected.proxy_route) == &config.query_backends[1]);
    assert(proxy_select_backend(&config, &selection, selected.proxy_route) == &config.query_backends[0]);
    assert(proxy_select_backend(&config, &independent_selection, selected.proxy_route) == &config.query_backends[0]);
    assert(request.target.data == original_target.data && request.target.length == original_target.length);
    request.path = slice("/hello");
    assert(http_select_route(&request, &selected, &allow) == 0 && selected.local_handler);
    assert(proxy_select_backend(&config, &selection, selected.proxy_route) == NULL);
    assert(selection.next_query_backend == 1);

    proxy_selection_destroy(&independent_selection);
    proxy_selection_destroy(&selection);
}

enum {
    CONCURRENT_WORKERS = 8,
    SELECTIONS_PER_WORKER = 1000
};

struct selection_worker_args {
    const struct proxy_config *config;
    struct proxy_selection_state *selection;
    size_t query_hits[2];
    size_t submission_hits;
};

/* Worker routine interleaving query and submission backend selections. */
static void *selection_worker_main(void *arg) {
    struct selection_worker_args *worker = arg;

    for (size_t i = 0; i < SELECTIONS_PER_WORKER; ++i) {
        const struct sockaddr_in *query_backend = proxy_select_backend(
            worker->config, worker->selection, PROXY_ROUTE_QUERY);
        if (query_backend == &worker->config->query_backends[0]) {
            ++worker->query_hits[0];
        } else if (query_backend == &worker->config->query_backends[1]) {
            ++worker->query_hits[1];
        } else {
            assert(0 && "unexpected query backend pointer");
        }

        const struct sockaddr_in *submission_backend = proxy_select_backend(
            worker->config, worker->selection, PROXY_ROUTE_SUBMISSIONS);
        assert(submission_backend == &worker->config->submission_backend);
        ++worker->submission_hits;
    }

    return NULL;
}

/* Verify NULL lifecycle handling for proxy_selection_init/destroy. */
static void test_selection_lifecycle_null(void) {
    errno = 0;
    assert(proxy_selection_init(NULL) == -1);
    assert(errno == EINVAL);
    proxy_selection_destroy(NULL);
}

/* Verify thread-safe 50:50 round-robin balance under concurrent query load. */
static void test_concurrent_query_selection(void) {
    struct proxy_config config = {
        .submission_backend = {.sin_port = htons(8001)},
        .query_backends = {{.sin_port = htons(8002)}, {.sin_port = htons(8003)}}
    };
    struct proxy_selection_state selection;
    assert(proxy_selection_init(&selection) == 0);

    pthread_t threads[CONCURRENT_WORKERS];
    struct selection_worker_args args[CONCURRENT_WORKERS];
    for (size_t i = 0; i < CONCURRENT_WORKERS; ++i) {
        args[i] = (struct selection_worker_args){
            .config = &config,
            .selection = &selection,
            .query_hits = {0, 0},
            .submission_hits = 0
        };
        assert(pthread_create(&threads[i], NULL, selection_worker_main, &args[i]) == 0);
    }

    size_t total_query_hits[2] = {0, 0};
    size_t total_submission_hits = 0;
    for (size_t i = 0; i < CONCURRENT_WORKERS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        total_query_hits[0] += args[i].query_hits[0];
        total_query_hits[1] += args[i].query_hits[1];
        total_submission_hits += args[i].submission_hits;
    }

    size_t expected_per_backend = (CONCURRENT_WORKERS * SELECTIONS_PER_WORKER) / 2;
    assert(total_query_hits[0] == expected_per_backend);
    assert(total_query_hits[1] == expected_per_backend);
    assert(total_submission_hits == CONCURRENT_WORKERS * SELECTIONS_PER_WORKER);
    assert(selection.next_query_backend == 0);

    proxy_selection_destroy(&selection);
}

static void test_protected_connection_options(void) {
    static const struct { const char *method, *path; } routes[] = {
        {"POST", "/api/submissions"}, {"GET", "/api/submission-status"},
        {"GET", "/api/hello"}
    };
    static const struct { const char *headers; int status; } cases[] = {
        {"Connection: Host\r\n", 400},
        {"cOnNeCtIoN:\tHoSt \t\r\n", 400},
        {"Connection: X-Hop, CONTENT-LENGTH, close\r\n", 400},
        {"Connection: transfer-encoding\r\n", 400},
        {"Connection: X-Hop\r\nConnection: close, Host\r\n", 400},
        {"Connection: close,\tTransfer-Encoding \t\r\n", 400},
        {"Connection: X-Host, Content-Length-X, X-Transfer-Encoding\r\n", 0},
        {"Connection: X-Hop, close\r\n", 0},
        {"", 0}
    };
    for (size_t r = 0; r < sizeof(routes) / sizeof(routes[0]); ++r) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            char header[512], original[512];
            int length = snprintf(header, sizeof(header),
                "%s %s HTTP/1.1\r\nHost: original.example\r\n"
                "Content-Length: 10\r\nExpect: 100-continue\r\n%s\r\n",
                routes[r].method, routes[r].path, cases[i].headers);
            assert(length > 0 && (size_t)length < sizeof(header));
            memcpy(original, header, (size_t)length);
            struct http_request request;
            assert(http_parse_request((const unsigned char *)header,
                                       (size_t)length, &request) == 0);
            struct request_route selected;
            const char *allow;
            assert(http_select_route(&request, &selected, &allow) == 0);
            assert(allow == NULL && selected.local_handler == NULL);
            assert(selected.proxy_route == (r == 0 ? PROXY_ROUTE_SUBMISSIONS : PROXY_ROUTE_QUERY));
            assert(proxy_validate_request(&request) == cases[i].status);
            assert(memcmp(header, original, (size_t)length) == 0);
        }
    }

    struct http_request local = {
        .method = slice("GET"), .path = slice("/hello"),
        .headers = slice("Connection: Host\r\n")
    };
    struct request_route selected;
    const char *allow;
    assert(http_select_route(&local, &selected, &allow) == 0);
    assert(selected.proxy_route == PROXY_ROUTE_LOCAL && selected.local_handler && !allow);
    local.path = slice("/api/submissions");
    assert(http_select_route(&local, &selected, &allow) == 405);
    assert(strcmp(allow, "POST") == 0);
    local.method = slice("HEAD");
    assert(http_select_route(&local, &selected, &allow) == 501);
}

int main(void) {
    char path[] = "/tmp/web-proxy-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    test_config(path);
    assert(unlink(path) == 0);
    test_route_table();
    test_selection_lifecycle_null();
    test_routes_and_selection();
    test_concurrent_query_selection();
    test_protected_connection_options();
    puts("[PASS] proxy config, method routing and round-robin selection");
    return 0;
}