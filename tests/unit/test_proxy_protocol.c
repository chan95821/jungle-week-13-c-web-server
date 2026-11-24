#define _POSIX_C_SOURCE 200809L
/* Exercise framing with real socketpairs; keep parser internals out of public APIs. */
#include "../../src/proxy_transfer.c"

#include <assert.h>

static int parse_response_text(const char *text, struct proxy_stream *stream) {
    memset(stream, 0, sizeof(*stream));
    stream->backend_fd = -1;
    stream->raw_header_length = strlen(text);
    assert(stream->raw_header_length <= sizeof(stream->raw_header));
    memcpy(stream->raw_header, text, stream->raw_header_length);
    return parse_backend_headers(stream);
}

static void test_response_headers(void) {
    struct proxy_stream stream;
    assert(parse_response_text(
        "HTTP/1.1 201 Custom reason\r\nContent-Length: 5\r\n"
        "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
        "Connection: close, X-Hop\r\nX-Hop: hidden\r\nX-End: keep\r\n\r\n",
        &stream) == 0);
    assert(stream.status == 201 && stream.framing == BACKEND_LENGTH && stream.content_length == 5);
    assert(build_client_response_headers(&stream) == 0);
    stream.client_header[stream.client_header_length] = 0;
    assert(strstr((char *)stream.client_header, "201 Custom reason"));
    assert(strstr((char *)stream.client_header, "Set-Cookie: a=1\r\nSet-Cookie: b=2"));
    assert(!strstr((char *)stream.client_header, "X-Hop:"));
    assert(strstr((char *)stream.client_header, "X-End: keep"));
    assert(strstr((char *)stream.client_header, "Via: 1.1 jungle-proxy"));
    assert(parse_response_text("HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n",
                               &stream) == 0);
    assert(stream.framing == BACKEND_NO_BODY && stream.content_length == 123);
    assert(parse_response_text("HTTP/1.1 204 No Content\r\n\r\n", &stream) == 0);
    assert(stream.framing == BACKEND_NO_BODY);
    assert(parse_response_text("HTTP/1.1 200 OK\r\n\r\n", &stream) == 0);
    assert(stream.framing == BACKEND_CLOSE);
    static const char *bad[] = {
        "HTTP/1.0 200 OK\r\n\r\n",
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
        assert(parse_response_text(bad[i], &stream) == -1);
        assert(errno == EPROTO);
    }
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
    close(pair[0]);
    close(pair[1]);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_response_headers();
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