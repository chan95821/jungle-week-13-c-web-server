#define _POSIX_C_SOURCE 200809L
#include "../../src/static_file.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(int root, const char *name, const void *data, size_t length) {
    int fd = openat(root, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, (const unsigned char *)data + written, length - written);
        assert(count > 0);
        written += (size_t)count;
    }
    assert(close(fd) == 0);
}

static void expect_file(int root, const char *url, int expected,
                         const char *type, const void *bytes, size_t length) {
    struct static_file file;
    /* 입력의 NUL 종료를 가정하지 않고 원본도 바꾸지 않는지 확인한다. */
    unsigned char input[512];
    size_t url_length = strlen(url);
    assert(url_length < sizeof(input));
    memset(input, 'X', sizeof(input));
    memcpy(input, url, url_length);
    assert(prepare_static_file(root, (struct http_slice){input, url_length}, &file) == expected);
    assert(memcmp(input, url, url_length) == 0 && input[url_length] == 'X');
    if (expected) {
        assert(file.fd == -1);
    } else {
        assert(file.fd >= 0 && file.length == length);
        assert(strcmp(file.content_type, type) == 0);
        unsigned char received[64];
        assert(length <= sizeof(received));
        size_t total = 0;
        while (total < length) {
            ssize_t count = read(file.fd, received + total, length - total);
            assert(count > 0);
            total += (size_t)count;
        }
        if (length) assert(memcmp(received, bytes, length) == 0);
        assert(close(file.fd) == 0);
    }
    assert(fcntl(root, F_GETFD) >= 0); /* 빌린 루트 FD가 닫히지 않았는지 확인. */
}

static void test_mime(void) {
    const struct { const char *path; const char *type; } cases[] = {
        {"a.html", "text/html; charset=utf-8"}, {"a.HTM", "text/html; charset=utf-8"},
        {"a.css", "text/css; charset=utf-8"}, {"a.JS", "text/javascript; charset=utf-8"},
        {"a.png", "image/png"}, {"a.JPG", "image/jpeg"}, {"a.jpeg", "image/jpeg"},
        {"a.gif", "image/gif"}, {"a.svg", "image/svg+xml"},
        {"a.xyz", "application/octet-stream"}, {"dir.css/file", "application/octet-stream"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert(strcmp(mime_from_path(cases[i].path), cases[i].type) == 0);
}

int main(void) {
    test_mime();
    char directory[] = "/tmp/week13-static-unit-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    int root = open_static_root(directory);
    assert(root >= 0);
    const unsigned char binary[] = {0, 255, '\r', '\n'};
    write_file(root, "index.html", "index", 5);
    write_file(root, "image.PNG", binary, sizeof(binary));
    write_file(root, "a+b.txt", "plus", 4);
    write_file(root, "a b.txt", "space", 5);
    write_file(root, "%2e%2e", "literal", 7);
    write_file(root, "empty", NULL, 0);
    write_file(root, "denied", "secret", 6);
    assert(fchmodat(root, "denied", 0, 0) == 0);
    assert(mkdirat(root, "css", 0700) == 0);
    write_file(root, "css/style.css", "css", 3);
    assert(symlinkat("index.html", root, "file-link") == 0);
    assert(symlinkat("css", root, "dir-link") == 0);
    assert(symlinkat("/etc/passwd", root, "outside-link") == 0);
    assert(mkfifoat(root, "pipe", 0600) == 0);

    expect_file(root, "/static/", 0, "text/html; charset=utf-8", "index", 5);
    expect_file(root, "/static/css/style.css", 0, "text/css; charset=utf-8", "css", 3);
    expect_file(root, "/static/image.PNG", 0, "image/png", binary, sizeof(binary));
    expect_file(root, "/static/a+b.txt", 0, "application/octet-stream", "plus", 4);
    expect_file(root, "/static/a%20b.txt", 0, "application/octet-stream", "space", 5);
    expect_file(root, "/static/%252e%252e", 0, "application/octet-stream", "literal", 7);
    expect_file(root, "/static/empty", 0, "application/octet-stream", NULL, 0);
    const struct { const char *url; int status; } bad[] = {
        {"/static/../index.html", 403}, {"/static/%2e%2e/index.html", 403},
        {"/static/missing/../index.html", 403}, {"/static/./index.html", 403},
        {"/static/css%2f..%2findex.html", 403},
        {"/static/file-link", 403}, {"/static/dir-link/style.css", 403},
        {"/static/outside-link", 403}, {"/static/css", 403}, {"/static/css/", 403},
        {"/static/pipe", 403}, {"/static/missing", 404}, {"/static/missing/a", 404},
        {"/static/index.html/a", 404}, {"/static/a%00b", 400}, {"/static/a%0ab", 400},
        {"/static/a%5cb", 400}, {"/static/a%GG", 400}, {"/static/a%", 400},
        {"/static//index.html", 400}, {"/static/%2findex.html", 400}, {"/other/index.html", 400}
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        expect_file(root, bad[i].url, bad[i].status, NULL, NULL, 0);
    if (geteuid() != 0) expect_file(root, "/static/denied", 403, NULL, NULL, 0);
    else puts("[SKIP] root의 실제 읽기 권한 거부 검사는 비루트 E2E에서 수행");
    struct static_file file;
    const char *url = "/static/index.html";
    assert(prepare_static_file(-1, (struct http_slice){(const unsigned char *)url, strlen(url)}, &file) == 500);
    assert(file.fd == -1);

    const char *files[] = {"index.html", "image.PNG", "a+b.txt", "a b.txt", "%2e%2e", "empty",
                           "denied", "css/style.css", "file-link", "dir-link", "outside-link", "pipe"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) assert(unlinkat(root, files[i], 0) == 0);
    assert(unlinkat(root, "css", AT_REMOVEDIR) == 0);
    assert(close(root) == 0 && rmdir(directory) == 0);
    puts("[PASS] test_static_file");
    return 0;
}
