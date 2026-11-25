#define _XOPEN_SOURCE 700
#include "handlers.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

struct file_body {
    int fd;
    unsigned char buffer[16384];
};

/* Read next chunk up to buffer size from opened static file FD. */
static enum body_result file_body_next(void *state, struct http_slice *chunk) {
    struct file_body *file = state;
    ssize_t count;
    do {
        count = read(file->fd, file->buffer, sizeof(file->buffer));
    } while (count < 0 && errno == EINTR);
    if (count < 0) return BODY_ERROR;
    if (count == 0) return BODY_END;
    *chunk = (struct http_slice){file->buffer, (size_t)count};
    return BODY_DATA;
}

/* Close static file FD and free file_body state. */
static void file_body_cleanup(void *state) {
    struct file_body *file = state;
    close(file->fd);
    free(file);
}

/* Map final filename extension (case-insensitive) to MIME type, or NULL if unsupported. */
static const char *file_type(const char *path) {
    const char *extension = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!extension || (slash && extension < slash)) return NULL;
    if (strcasecmp(extension, ".html") == 0) return "text/html";
    if (strcasecmp(extension, ".css") == 0) return "text/css";
    if (strcasecmp(extension, ".js") == 0) return "text/javascript";
    if (strcasecmp(extension, ".png") == 0) return "image/png";
    if (strcasecmp(extension, ".jpg") == 0 ||
        strcasecmp(extension, ".jpeg") == 0) return "image/jpeg";
    return NULL;
}

/* Validate MIME, canonical boundary inside allowed_directory, and regular file FD. */
static void prepare_file(const char *candidate, const char *allowed_directory,
                         struct router_response *response) {
    char *root = NULL, *boundary = NULL, *resolved = NULL;
    struct file_body *file = NULL;
    int fd = -1;
    int status = 500;
    struct stat info;

    const char *type = file_type(candidate);
    if (!type) { status = 404; goto fail; }

    root = realpath("public", NULL);
    if (!root) { status = router_path_error_status(errno); goto fail; }
    boundary = realpath(allowed_directory, NULL);
    if (!boundary) { status = router_path_error_status(errno); goto fail; }
    if (!router_inside_directory(root, boundary)) { status = 404; goto fail; }

    resolved = realpath(candidate, NULL);
    if (!resolved) { status = router_path_error_status(errno); goto fail; }
    if (!router_inside_directory(boundary, resolved)) {
        status = 404;
        goto fail;
    }

    /* O_NONBLOCK prevents opening a FIFO from blocking before fstat rejects it.
     * The path may still change between realpath and open (TOCTOU). */
    fd = open(resolved, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { status = router_path_error_status(errno); goto fail; }
    if (fstat(fd, &info) < 0) goto fail;
    if (!S_ISREG(info.st_mode)) { status = 404; goto fail; }
    if (info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX) goto fail;

    file = malloc(sizeof(*file));
    if (!file) goto fail;
    file->fd = fd;

    *response = (struct router_response){
        .status = 200,
        .content_type = type,
        .length = (size_t)info.st_size,
        .body_provider = {.next_func = file_body_next, .cleanup_func = file_body_cleanup, .state = file}
    };

    free(root);
    free(boundary);
    free(resolved);
    return;

fail:
    if (fd >= 0) close(fd);
    free(file);
    free(root);
    free(boundary);
    free(resolved);
    router_prepare_error(response, status, NULL);
}

/* Serve fixed demo page public/pages/cafe.html for GET /cafe. */
void prepare_cafe(const struct http_request *request,
                  struct router_response *response) {
    (void)request;
    prepare_file("public/pages/cafe.html", "public", response);
}

/* Serve fixed demo page public/pages/menu.html for GET /menu. */
void prepare_menu(const struct http_request *request,
                  struct router_response *response) {
    (void)request;
    prepare_file("public/pages/menu.html", "public", response);
}

/* Reject '%' in path with 400; otherwise resolve under public/assets. */
void prepare_assets(const struct http_request *request,
                    struct router_response *response) {
    if (memchr(request->path.data, '%', request->path.length)) {
        router_prepare_error(response, 400, NULL);
        return;
    }

    char *candidate = malloc(sizeof("public") + request->path.length);
    if (!candidate) {
        router_prepare_error(response, 500, NULL);
        return;
    }

    memcpy(candidate, "public", sizeof("public") - 1);
    memcpy(candidate + sizeof("public") - 1, request->path.data, request->path.length);
    candidate[sizeof("public") - 1 + request->path.length] = '\0';

    prepare_file(candidate, "public/assets", response);
    free(candidate);
}
