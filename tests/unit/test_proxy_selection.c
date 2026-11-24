#define _POSIX_C_SOURCE 200809L
#include "../../src/proxy.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
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

static void test_routes_and_selection(void) {
    struct proxy_config config = {
        .submission_backend = {.sin_port = htons(8001)},
        .query_backends = {{.sin_port = htons(8002)}, {.sin_port = htons(8003)}}
    };
    struct proxy_selection_state selection = {0}, independent_selection = {0};
    struct http_request request = {
        .method = slice("GET"),
        .path = slice("/api/submission-status"),
        .target = slice("/api/submission-status?id=7")
    };
    struct http_slice original_target = request.target;
    const char *allow;
    enum proxy_route route;
    assert(proxy_match_request(&request, &route, &allow) == 0);
    assert(route == PROXY_ROUTE_QUERY && allow == NULL);
    assert(proxy_select_backend(&config, &selection, route) == &config.query_backends[0]);
    assert(selection.next_query_backend == 1);

    request.method = slice("POST");
    request.path = slice("/api/submissions");
    assert(proxy_match_request(&request, &route, &allow) == 0);
    assert(route == PROXY_ROUTE_SUBMISSIONS);
    assert(proxy_select_backend(&config, &selection, route) == &config.submission_backend);
    assert(selection.next_query_backend == 1);

    request.method = slice("GET");
    request.path = slice("/api/hello");
    assert(proxy_match_request(&request, &route, &allow) == 0);
    assert(proxy_select_backend(&config, &selection, route) == &config.query_backends[1]);
    assert(proxy_select_backend(&config, &selection, route) == &config.query_backends[0]);
    assert(proxy_select_backend(&config, &independent_selection, route) ==
           &config.query_backends[0]);
    assert(request.target.data == original_target.data);
    assert(request.target.length == original_target.length);

    request.method = slice("POST");
    assert(proxy_match_request(&request, &route, &allow) == 405);
    assert(strcmp(allow, "GET") == 0 && route == PROXY_ROUTE_LOCAL);
    request.method = slice("GET");
    request.path = slice("/api/submissions");
    assert(proxy_match_request(&request, &route, &allow) == 405);
    assert(strcmp(allow, "POST") == 0);
    request.method = slice("HEAD");
    assert(proxy_match_request(&request, &route, &allow) == 501);

    static const char *local_paths[] = {
        "/", "/hello", "/echo", "/assets/a.css", "/greet", "/api/hello/",
        "/api/submissions-extra", "/api/unknown"
    };
    for (size_t i = 0; i < sizeof(local_paths) / sizeof(local_paths[0]); ++i) {
        request.path = slice(local_paths[i]);
        assert(proxy_match_request(&request, &route, &allow) == 0);
        assert(route == PROXY_ROUTE_LOCAL && allow == NULL);
        assert(proxy_select_backend(&config, &selection, route) == NULL);
    }
    assert(selection.next_query_backend == 1);
}

int main(void) {
    char path[] = "/tmp/web-proxy-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    test_config(path);
    assert(unlink(path) == 0);
    test_routes_and_selection();
    puts("[PASS] proxy config, method routing and round-robin selection");
    return 0;
}