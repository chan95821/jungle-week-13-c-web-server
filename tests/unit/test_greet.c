#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/greet.h"

static void check(const char *target, int status, const char *body) {
    char wire[REQ_HEADER_MAX_BYTES];
    int count = snprintf(wire, sizeof(wire), "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", target);
    assert(count > 0 && (size_t)count < sizeof(wire));
    struct http_request request;
    assert(http_parse_request((const unsigned char *)wire, (size_t)count, &request) == 0);
    unsigned char *owned;
    struct router_response response = greet_prepare_response(&request, &owned);
    assert(response.status == status);
    if (status == 200) {
        assert(owned && response.body == owned);
        assert(strcmp(response.content_type, "text/plain; charset=utf-8") == 0);
        assert(response.length == strlen(body));
        assert(memcmp(response.body, body, strlen(body)) == 0);
    } else assert(!owned);
    free(owned);
}

int main(void) {
    check("/greet", 200, "Hello, World!\n");
    check("/greet?&&x=1", 200, "Hello, World!\n");
    check("/greet?name=Alice", 200, "Hello, Alice!\n");
    check("/greet?na%6De=Alice+Bob", 200, "Hello, Alice Bob!\n");
    check("/greet?name=A%26B%3DC", 200, "Hello, A&B=C!\n");
    check("/greet?name=%2526", 200, "Hello, %26!\n");
    check("/greet?name=%ED%95%9C%EA%B8%80", 200, "Hello, \xed\x95\x9c\xea\xb8\x80!\n");
    check("/greet?name=%F0%9F%98%80", 200, "Hello, \xf0\x9f\x98\x80!\n");
    check("/greet?Name=Alice", 200, "Hello, World!\n");
    check("/greet?name=%C2%A0", 200, "Hello, \xc2\xa0!\n");
    check("/greet?name=%F4%8F%BF%BF", 200, "Hello, \xf4\x8f\xbf\xbf!\n");
    const char *bad[] = {
        "/greet?name", "/greet?name=", "/greet?name=A&na%6De=B",
        "/greet?name=%00", "/greet?name=%0a", "/greet?name=%7f",
        "/greet?x=%C2%80", "/greet?%C2%9F=x", "/greet?x=%ff",
        "/greet?name=%C0%AF", "/greet?name=%E0%80%AF",
        "/greet?name=%ED%A0%80", "/greet?name=%F4%90%80%80",
        "/greet?name=%E2%82", "/greet?name=%80",
        "/greet?%ff=ignored", "/greet?x=%E2%28%A1"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) check(bad[i], 400, NULL);

    /* Test direct helper safety too; wire parser already rejects malformed %. */
    const char *target = "/greet?unknown=%G0";
    struct http_request request = {
        .target = {(const unsigned char *)target, strlen(target)},
        .path = {(const unsigned char *)target, 6}
    };
    unsigned char *owned;
    assert(greet_prepare_response(&request, &owned).status == 400 && !owned);
    puts("[PASS] greet query policy (current UTF-8/control range)");
    return 0;
}