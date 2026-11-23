#define _POSIX_C_SOURCE 200809L
#include "static.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int static_open_root(const char *path) {
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int static_decode_path(struct http_slice path, char *decoded, size_t capacity) {
    static const char prefix[] = "/static/";
    if (path.length < sizeof(prefix) - 1 ||
        memcmp(path.data, prefix, sizeof(prefix) - 1) != 0) return 404;
    size_t length = 0;
    for (size_t i = sizeof(prefix) - 1; i < path.length; ++i) {
        unsigned char c = path.data[i];
        if (c == '%') {
            if (path.length - i < 3) return 400;
            int high = hex_value(path.data[i + 1]);
            int low = hex_value(path.data[i + 2]);
            if (high < 0 || low < 0) return 400;
            c = (unsigned char)(high * 16 + low);
            if (c == '/') return 400;
            i += 2;
        }
        if (c < 32 || c == 127 || c == '\\') return 400;
        if (length + 1 >= capacity) return 414;
        decoded[length++] = (char)c;
    }
    if (!capacity) return 414;
    decoded[length] = '\0';
    if (!length) return 403;

    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i != length && decoded[i] != '/') continue;
        size_t count = i - start;
        if (!count && i != length) return 400;
        if ((count == 1 && decoded[start] == '.') ||
            (count == 2 && decoded[start] == '.' && decoded[start + 1] == '.')) return 403;
        start = i + 1;
    }
    return decoded[length - 1] == '/' ? 403 : 0;
}

static bool extension_equal(const char *left, const char *right) {
    while (*left && *right) {
        unsigned char c = (unsigned char)*left++;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)*right++) return false;
    }
    return *left == *right;
}

const char *static_mime_type(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *extension = strrchr(name, '.');
    if (!extension) return "application/octet-stream";
    ++extension;
    if (extension_equal(extension, "html") || extension_equal(extension, "htm")) return "text/html";
    if (extension_equal(extension, "css")) return "text/css";
    if (extension_equal(extension, "js")) return "text/javascript";
    if (extension_equal(extension, "png")) return "image/png";
    if (extension_equal(extension, "jpg") || extension_equal(extension, "jpeg")) return "image/jpeg";
    return "application/octet-stream";
}

static int file_error(int error) {
    switch (error) {
    case ENOENT: case ENOTDIR: return 404;
    case EACCES: case EPERM: case ELOOP: return 403;
    case ENAMETOOLONG: return 414;
    default: return 500;
    }
}

int static_open_file(int rootfd, struct http_slice path, int *filefd,
                     uintmax_t *length, const char **content_type) {
    char decoded[REQ_HEADER_MAX_BYTES + 1];
    *filefd = -1;
    int status = static_decode_path(path, decoded, sizeof(decoded));
    if (status) return status;
    *content_type = static_mime_type(decoded);
    int directory = rootfd; /* Borrow root; own every subsequently opened FD. */
    char *component = decoded;
    for (;;) {
        char *slash = strchr(component, '/');
        if (slash) *slash = '\0';
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
        if (slash) flags |= O_DIRECTORY;
        int next = openat(directory, component, flags);
        if (next < 0) {
            int error = errno;
            /* Some systems report ENOTDIR for an intermediate symlink. */
            struct stat link_info;
            bool link = fstatat(directory, component, &link_info, AT_SYMLINK_NOFOLLOW) == 0 &&
                        S_ISLNK(link_info.st_mode);
            if (directory != rootfd) close(directory);
            return link ? 403 : file_error(error);
        }
        if (directory != rootfd) close(directory);
        if (slash) {
            directory = next;
            component = slash + 1;
            continue;
        }
        struct stat info;
        if (fstat(next, &info) < 0) {
            int error = errno;
            close(next);
            return file_error(error);
        }
        if (!S_ISREG(info.st_mode)) { close(next); return 403; }
        if (info.st_size < 0) { close(next); return 500; }
        *length = (uintmax_t)info.st_size;
        *filefd = next;
        return 0;
    }
}