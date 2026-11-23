#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../src/static.h"

static struct http_slice slice(const char *text) {
    return (struct http_slice){(const unsigned char *)text, strlen(text)};
}

static void paths(void) {
    const struct { const char *path; int status; const char *decoded; } cases[] = {
        {"/static/a%20b.txt", 0, "a b.txt"},
        {"/static/a+b.css", 0, "a+b.css"},
        {"/static/%252e%252e/file", 0, "%2e%2e/file"},
        {"/static/%ED%95%9C", 0, "\xed\x95\x9c"},
        {"/static", 404, NULL}, {"/static/", 403, NULL},
        {"/static/dir/", 403, NULL}, {"/static/a//b", 400, NULL},
        {"/static//a", 400, NULL}, {"/static/./a", 403, NULL},
        {"/static/a/../b", 403, NULL}, {"/static/%2e%2e/b", 403, NULL},
        {"/static/%2fetc", 400, NULL}, {"/static/a%5Cb", 400, NULL},
        {"/static/a%00b", 400, NULL}, {"/static/%1f", 400, NULL},
        {"/static/%7f", 400, NULL}, {"/static/%GG", 400, NULL}
    };
    char output[REQ_HEADER_MAX_BYTES + 1];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(static_decode_path(slice(cases[i].path), output, sizeof(output)) == cases[i].status);
        if (!cases[i].status) assert(strcmp(output, cases[i].decoded) == 0);
    }
    assert(strcmp(static_mime_type("dir.with.dot/IMAGE.JPG"), "image/jpeg") == 0);
    assert(strcmp(static_mime_type("x.HTM"), "text/html") == 0);
    assert(strcmp(static_mime_type("x.css"), "text/css") == 0);
    assert(strcmp(static_mime_type("x.js"), "text/javascript") == 0);
    assert(strcmp(static_mime_type("x.png"), "image/png") == 0);
    assert(strcmp(static_mime_type("x.txt"), "application/octet-stream") == 0);
    assert(strcmp(static_mime_type("dir.html/no-extension"), "application/octet-stream") == 0);
}

int main(void) {
    paths();
    char directory[] = "/tmp/web-static-XXXXXX";
    assert(mkdtemp(directory));
    int root = static_open_root(directory);
    assert(root >= 0);
    const unsigned char binary[] = {0, 1, 255, '\r', '\n'};
    int file = openat(root, "a.png", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(file >= 0 && write(file, binary, sizeof(binary)) == (ssize_t)sizeof(binary));
    assert(close(file) == 0);
    assert(mkdirat(root, "dir", 0700) == 0);
    assert(symlinkat("a.png", root, "inside") == 0);
    assert(symlinkat("/etc/passwd", root, "outside") == 0);
    assert(symlinkat("dir", root, "dirlink") == 0);
    assert(symlinkat("missing", root, "broken") == 0);
    assert(mkfifoat(root, "pipe", 0600) == 0);
    uintmax_t length;
    const char *type;
    assert(static_open_file(root, slice("/static/a.png"), &file, &length, &type) == 0);
    assert(length == sizeof(binary) && strcmp(type, "image/png") == 0);
    unsigned char received[sizeof(binary)];
    assert(read(file, received, sizeof(received)) == (ssize_t)sizeof(received));
    assert(memcmp(binary, received, sizeof(binary)) == 0);
    /* Same-filesystem publish: old FD remains the old inode, new open gets new. */
    int replacement = openat(root, "new.tmp", O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(replacement >= 0 && write(replacement, "new", 3) == 3);
    assert(close(replacement) == 0 && renameat(root, "new.tmp", root, "a.png") == 0);
    assert(lseek(file, 0, SEEK_SET) == 0);
    assert(read(file, received, sizeof(received)) == (ssize_t)sizeof(received));
    assert(memcmp(binary, received, sizeof(binary)) == 0 && close(file) == 0);
    assert(static_open_file(root, slice("/static/a.png"), &file, &length, &type) == 0);
    assert(length == 3 && read(file, received, sizeof(received)) == 3);
    assert(memcmp(received, "new", 3) == 0 && close(file) == 0);
    const struct { const char *path; int status; } bad[] = {
        {"/static/missing", 404}, {"/static/a.png/child", 404},
        {"/static/dir", 403}, {"/static/inside", 403},
        {"/static/outside", 403}, {"/static/dirlink/child", 403},
        {"/static/broken", 403}, {"/static/pipe", 403}
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(static_open_file(root, slice(bad[i].path), &file, &length, &type) == bad[i].status);
        assert(file == -1 && fcntl(root, F_GETFD) >= 0);
    }
    assert(unlinkat(root, "a.png", 0) == 0);
    assert(unlinkat(root, "dir", AT_REMOVEDIR) == 0);
    const char *entries[] = {"inside", "outside", "dirlink", "broken", "pipe"};
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i)
        assert(unlinkat(root, entries[i], 0) == 0);
    assert(close(root) == 0 && rmdir(directory) == 0);
    puts("[PASS] static decoding, MIME, regular file and link rejection");
    return 0;
}