#define _POSIX_C_SOURCE 200809L
/* Exercise framing with real socketpairs; keep parser internals out of public APIs. */
#include "../../src/proxy_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>

static int parse_response_text(const char *text, struct proxy_stream *stream,
                               struct proxy_response_headers *headers) {
    memset(stream, 0, sizeof(*stream));
    memset(headers, 0, sizeof(*headers));
    stream->backend_fd = -1;
    headers->raw_header_length = strlen(text);
    assert(headers->raw_header_length <= sizeof(headers->raw_header));
    memcpy(headers->raw_header, text, headers->raw_header_length);
    return parse_backend_headers(stream, headers);
}

static void test_response_headers(void) {
    struct proxy_response_headers headers;
    struct proxy_stream stream;
    assert(parse_response_text(
        "HTTP/1.1 201 Custom reason\r\nContent-Length: 5\r\n"
        "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
        "Connection: close, X-Hop\r\nX-Hop: hidden\r\nX-End: keep\r\n\r\n",
        &stream, &headers) == 0);
    assert(stream.status == 201 && stream.framing == BACKEND_LENGTH && stream.content_length == 5);
    assert(build_client_response_headers(&stream, &headers) == 0);
    headers.client_header[headers.client_header_length] = 0;
    assert(strstr((char *)headers.client_header, "201 Custom reason"));
    assert(strstr((char *)headers.client_header, "Set-Cookie: a=1\r\nSet-Cookie: b=2"));
    assert(!strstr((char *)headers.client_header, "X-Hop:"));
    assert(strstr((char *)headers.client_header, "X-End: keep"));
    assert(strstr((char *)headers.client_header, "Via: 1.1 jungle-proxy"));
    assert(parse_response_text("HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n",
                               &stream, &headers) == 0);
    assert(stream.framing == BACKEND_NO_BODY && stream.content_length == 123);
    assert(parse_response_text("HTTP/1.1 204 No Content\r\n\r\n", &stream, &headers) == 0);
    assert(stream.framing == BACKEND_NO_BODY);
    assert(parse_response_text("HTTP/1.1 200 OK\r\n\r\n", &stream, &headers) == 0);
    assert(stream.framing == BACKEND_CLOSE);
    static const char *bad[] = {
        "HTTP/1.1 600 Invalid\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1,1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999999999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection: close,\r\n\r\n",
        "HTTP/1.1 200 OK\r\n Bad: folded\r\n\r\n",
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        errno = 0;
        assert(parse_response_text(bad[i], &stream, &headers) == -1);
        assert(errno == EPROTO);
    }
}

static void test_http10_response_headers(void) {
    struct proxy_response_headers headers;
    static const struct {
        const char *wire;
        enum backend_framing framing;
        const char *client_status;
    } cases[] = {
        {"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\n",
         BACKEND_LENGTH, "HTTP/1.1 200 OK\r\n"},
        {"HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n",
         BACKEND_LENGTH, "HTTP/1.1 200 OK\r\n"},
        {"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n",
         BACKEND_CLOSE, "HTTP/1.1 200 OK\r\n"},
        {"HTTP/1.0 204 No Content\r\n\r\n",
         BACKEND_NO_BODY, "HTTP/1.1 204 No Content\r\n"},
        {"HTTP/1.0 304 Not Modified\r\nContent-Length: 123\r\n\r\n",
         BACKEND_NO_BODY, "HTTP/1.1 304 Not Modified\r\n"},
        {"HTTP/1.0 409 Legacy conflict\r\nContent-Length: 8\r\n"
         "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\n",
         BACKEND_LENGTH, "HTTP/1.1 409 Legacy conflict\r\n"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct proxy_stream stream;
        assert(parse_response_text(cases[i].wire, &stream, &headers) == 0);
        assert(stream.framing == cases[i].framing);
        assert(build_client_response_headers(&stream, &headers) == 0);
        assert(!memcmp(headers.client_header, cases[i].client_status,
                       strlen(cases[i].client_status)));
        assert(!memcmp(headers.raw_header, cases[i].wire, strlen(cases[i].wire)));
        headers.client_header[headers.client_header_length] = 0;
        const char *output = (const char *)headers.client_header;
        assert(strstr(output, "Via: 1.1 jungle-proxy\r\n"));
        if (stream.framing == BACKEND_CLOSE)
            assert(strstr(output, "Transfer-Encoding: chunked\r\n"));
        if (stream.status == 204) {
            assert(!strstr(output, "Content-Type:"));
            assert(!strstr(output, "Content-Length:"));
            assert(!strstr(output, "Transfer-Encoding:"));
        }
        if (stream.status == 304) {
            assert(strstr(output, "Content-Length: 123\r\n"));
            assert(!strstr(output, "Transfer-Encoding:"));
        }
        if (stream.status == 409)
            assert(strstr(output, "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"));
    }
    static const char *bad[] = {
        "HTTP/1.0 100 Continue\r\n\r\n",
        "HTTP/1.0 103 Early Hints\r\n\r\n",
        "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 999999999999999999999999999\r\n\r\n",
        "HTTP/1.0 204 No Content\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.2 200 OK\r\n\r\n",
        "HTTP/2.0 200 OK\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        struct proxy_stream stream;
        assert(parse_response_text(bad[i], &stream, &headers) == -1);
        assert(errno == EPROTO);
    }
}

static void test_forwarded_request_lengths(void) {
    static const unsigned char binary[] = {'a', 0, 'b'};
    static const struct {
        const char *method, *path, *length_header;
        size_t body_length;
        bool expected_length;
    } cases[] = {
        {"GET", "/api/submission-status", "", 0, false},
        {"GET", "/api/submission-status", "Content-Length: 0\r\n", 0, true},
        {"GET", "/api/submission-status", "Content-Length: 3\r\n", 3, true},
        {"POST", "/api/submissions", "", 0, true},
        {"POST", "/api/submissions", "Content-Length: 0\r\n", 0, true},
        {"POST", "/api/submissions", "Content-Length: 3\r\n", 3, true}
    };
    const struct sockaddr_in backend = {
        .sin_family = AF_INET, .sin_port = htons(8001),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char input[512];
        int length = snprintf(input, sizeof(input),
            "%s %s HTTP/1.1\r\nHost: original.example\r\n%s\r\n",
            cases[i].method, cases[i].path, cases[i].length_header);
        assert(length > 0 && (size_t)length < sizeof(input));
        struct http_request request;
        assert(http_parse_request((const unsigned char *)input, (size_t)length, &request) == 0);
        request.body = (struct http_slice){binary, cases[i].body_length};
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        struct proxy_stream stream = {.backend_fd = pair[0]};
        assert(forward_request_to_backend(&stream, &backend, &request) == 0);
        assert(shutdown(pair[0], SHUT_WR) == 0);
        unsigned char wire[1024];
        size_t received = 0;
        ssize_t count;
        while ((count = recv(pair[1], wire + received, sizeof(wire) - received, 0)) > 0)
            received += (size_t)count;
        assert(count == 0 && received < sizeof(wire));
        size_t header_end = 0;
        for (size_t n = 0; n + 4 <= received; ++n)
            if (!memcmp(wire + n, "\r\n\r\n", 4)) { header_end = n + 4; break; }
        assert(header_end != 0);
        struct http_request forwarded;
        assert(http_parse_request(wire, header_end, &forwarded) == 0);
        assert(forwarded.has_content_length == cases[i].expected_length);
        assert(forwarded.content_length == cases[i].body_length);
        assert(http_slice_equal(forwarded.host, "127.0.0.1:8001"));
        assert(received - header_end == cases[i].body_length);
        assert(!memcmp(wire + header_end, binary, cases[i].body_length));
        size_t offset = 0, via_count = 0, connection_count = 0;
        struct http_field field;
        while (http_next_field(forwarded.headers, &offset, &field) > 0) {
            if (http_field_name_equal(field.name, "via")) {
                assert(http_slice_equal(field.value, "1.1 jungle-proxy"));
                ++via_count;
            }
            if (http_field_name_equal(field.name, "connection")) {
                assert(http_slice_equal(field.value, "close"));
                ++connection_count;
            }
        }
        assert(via_count == 1 && connection_count == 1);
        close(pair[0]);
        close(pair[1]);
    }
}

static void test_informational_connection_headers(void) {
    struct proxy_response_headers headers;
    static const int statuses[] = {102, 103, 199};
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        char input[256];
        int length = snprintf(input, sizeof(input),
            "HTTP/1.1 %d Interim\r\nLink: </a.css>\r\n"
            "Connection: close, X-Hop\r\nX-Hop: remove\r\n\r\n", statuses[i]);
        assert(length > 0 && (size_t)length < sizeof(input));
        struct proxy_stream stream;
        assert(parse_response_text(input, &stream, &headers) == 0);
        assert(build_client_response_headers(&stream, &headers) == 0);
        headers.client_header[headers.client_header_length] = 0;
        const char *output = (const char *)headers.client_header;
        assert(strstr(output, "Link: </a.css>\r\n"));
        assert(strstr(output, "Via: 1.1 jungle-proxy\r\n"));
        assert(!strstr(output, "Connection:"));
        assert(!strstr(output, "X-Hop:"));
        assert(!strstr(output, "Content-Length:"));
        assert(!strstr(output, "Transfer-Encoding:"));
    }
    struct proxy_stream final;
    assert(parse_response_text("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", &final, &headers) == 0);
    assert(build_client_response_headers(&final, &headers) == 0);
    headers.client_header[headers.client_header_length] = 0;
    assert(strstr((char *)headers.client_header, "Connection: close\r\n"));
}

static void test_chunk_metadata(void) {
    size_t size;
    assert(parse_chunk_size((struct http_slice){(const unsigned char *)"a;foo=\"bar\"", 11},
                            &size) == 0 && size == 10);
    static const char *bad[] = {"", "-1", "z", "1;=x", "1;x=\"open", "1;x=", "1\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(parse_chunk_size((struct http_slice){
            (const unsigned char *)bad[i], strlen(bad[i])}, &size) == -1);
    unsigned char overflow[2 * sizeof(size_t) + 2];
    memset(overflow, 'f', sizeof(overflow));
    assert(parse_chunk_size((struct http_slice){overflow, sizeof(overflow)}, &size) == -1);
}

static void check_body(const unsigned char *wire, size_t wire_length,
                        enum backend_framing framing, size_t content_length,
                        const unsigned char *expected, size_t expected_length,
                        bool expect_error, bool expect_trailer) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(send_all(pair[1], wire, wire_length) == 0);
    assert(shutdown(pair[1], SHUT_WR) == 0);
    struct proxy_stream stream = {
        .backend_fd = pair[0], .body_started = true, .framing = framing,
        .remaining = content_length
    };
    assert(make_deadline(&stream.body_deadline, PROXY_BODY_SECONDS) == 0);
    unsigned char received[256];
    size_t used = 0;
    enum body_result result;
    for (;;) {
        struct http_slice chunk;
        result = read_backend_body_chunk(&stream, &chunk);
        if (result != BODY_DATA) break;
        assert(chunk.length <= sizeof(received) - used);
        memcpy(received + used, chunk.data, chunk.length);
        used += chunk.length;
    }
    assert(result == (expect_error ? BODY_ERROR : BODY_END));
    assert(used == expected_length);
    assert(!memcmp(received, expected, used));
    if (expect_trailer) {
        static const char trailer[] = "X-End: yes\r\n";
        assert(stream.trailers.length == sizeof(trailer) - 1);
        assert(!memcmp(stream.trailers.data, trailer, sizeof(trailer) - 1));
    }
    free(stream.trailer_buffer);
    close(pair[0]);
    close(pair[1]);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_response_headers();
    test_http10_response_headers();
    test_forwarded_request_lengths();
    test_informational_connection_headers();
    test_chunk_metadata();
    static const unsigned char chunked[] =
        "3;kind=\"test\"\r\na\0b\r\n2\r\ncd\r\n0\r\nX-End: yes\r\n\r\n";
    static const unsigned char decoded[] = "a\0bcd";
    check_body(chunked, sizeof(chunked) - 1, BACKEND_CHUNKED, 0,
                decoded, sizeof(decoded) - 1, false, true);
    check_body((const unsigned char *)"abc", 3, BACKEND_LENGTH, 5,
                (const unsigned char *)"abc", 3, true, false);
    check_body((const unsigned char *)"abcEXTRA", 8, BACKEND_LENGTH, 3,
                (const unsigned char *)"abc", 3, false, false);
    check_body(decoded, sizeof(decoded) - 1, BACKEND_CLOSE, 0,
                decoded, sizeof(decoded) - 1, false, false);
    static const unsigned char bad[] = "3\r\nabc\r\nZ\r\n";
    check_body(bad, sizeof(bad) - 1, BACKEND_CHUNKED, 0,
                (const unsigned char *)"abc", 3, true, false);
    static const unsigned char bad_trailer[] = "0\r\nContent-Length: 3\r\n\r\n";
    check_body(bad_trailer, sizeof(bad_trailer) - 1, BACKEND_CHUNKED, 0,
                (const unsigned char *)"", 0, true, false);
    puts("[PASS] proxy response headers, binary framing and incomplete streams");
    return 0;
}