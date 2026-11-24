#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/proxy.h"

/* 해석 결과는 입력을 빌리며 백엔드 상태와 본문 길이를 바꾸지 않는다. */
static void parses_backend_metadata(void) {
    unsigned char wire[] = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json \t\r\n"
                           "Content-Length: 005\r\nConnection: close\r\n\r\n";
    unsigned char original[sizeof(wire)];
    memcpy(original, wire, sizeof(wire));
    struct http_response_headers parsed;
    assert(http_parse_response_headers(wire, sizeof(wire) - 1, &parsed) == 0);
    assert(parsed.status == 404 && parsed.content_length == 5 && parsed.has_content_length);
    assert(http_slice_equal(parsed.content_type, "application/json"));
    assert(!parsed.has_transfer_encoding);
    assert(validate_backend_response_headers(&parsed) == 0);
    assert(memcmp(wire, original, sizeof(wire)) == 0);
    assert(parsed.content_type.data >= wire && parsed.content_type.data < wire + sizeof(wire));
}

/* 상태 줄·헤더 형식 오류와 정상 형식이지만 미지원인 응답을 구분한다. */
static void checks_response_boundaries(void) {
    static const struct { const char *wire; int parse_status, relay_status; } cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, 0},
        {"HTTP/1.0 500 Error\r\nContent-Length: 1\r\n\r\n", 0, 0},
        {"HTTP/1.1 200 OK\r\n\r\n", 0, 502},
        {"HTTP/1.1 204 No Content\r\n\r\n", 0, 0},
        {"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", 0, 502},
        {"HTTP/1.1 304 Not Modified\r\nContent-Length: 99\r\n\r\n", 0, 0},
        {"HTTP/1.1 103 Early Hints\r\n\r\n", 0, 502},
        {"HTTP/1.1 103 Early Hints\r\nContent-Length: 0\r\n\r\n", 0, 502},
        {"HTTP/1.1 101 Switching Protocols\r\n\r\n", 0, 502},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", 0, 502},
        {"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: content-length\r\n\r\n", 0, 502},
        {"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: Transfer-Encoding\r\n\r\n", 0, 502},
        {"HTTP/1.1 20x OK\r\nContent-Length: 0\r\n\r\n", 502, 0},
        {"HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n", 502, 0},
        {"HTTP/1.1 600 Error\r\nContent-Length: 0\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\nContent-Type: a\r\nContent-Type: b\r\nContent-Length: 0\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\nX : a\r\nContent-Length: 0\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\n Folded: a\r\nContent-Length: 0\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\nX: a\001b\r\nContent-Length: 0\r\n\r\n", 502, 0},
        {"HTTP/1.1 200 OK\r\nConnection: close,\r\nContent-Length: 0\r\n\r\n", 502, 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct http_response_headers parsed;
        assert(http_parse_response_headers((const unsigned char *)cases[i].wire,
                                            strlen(cases[i].wire), &parsed) == cases[i].parse_status);
        if (cases[i].parse_status == 0) {
            assert(validate_backend_response_headers(&parsed) == cases[i].relay_status);
        }
    }
}

/* 파서 자체와 프록시 정책만 검증하며 본문 공급자·클라이언트 중계는 범위 밖이다. */
int main(void) {
    parses_backend_metadata();
    checks_response_boundaries();
    puts("[PASS] test_http_response");
    return 0;
}
