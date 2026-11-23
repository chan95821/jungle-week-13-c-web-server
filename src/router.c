#define _XOPEN_SOURCE 700
#include "router.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum route_match { ROUTE_EXACT, ROUTE_PREFIX };

static void prepare_hello(const struct http_request *request,
                          struct router_response *response);
static void prepare_echo(const struct http_request *request,
                         struct router_response *response);
static void prepare_cafe(const struct http_request *request,
                         struct router_response *response);
static void prepare_menu(const struct http_request *request,
                         struct router_response *response);
static void prepare_assets(const struct http_request *request,
                           struct router_response *response);

static enum body_result memory_body_next(void *state, struct http_slice *chunk) {
    struct http_slice *remaining = state;
    if (remaining->length == 0) return BODY_END;
    *chunk = *remaining;
    remaining->length = 0;
    return BODY_DATA;
}

static void prepare_memory_provider(struct router_response *response) {
    response->memory_body = (struct http_slice){response->body, response->length};
    response->body_provider = (struct body_provider){
        .next_func = memory_body_next,
        .cleanup_func = NULL,
        .state = &response->memory_body
    };
}

struct route_rule {
    const char *path;
    const char *method;
    enum route_match match;
    router_handler route_handler_func;
};

static const struct route_rule routes[] = {
    {"/", "GET", ROUTE_EXACT, prepare_hello},
    {"/hello", "GET", ROUTE_EXACT, prepare_hello},
    {"/echo", "POST", ROUTE_EXACT, prepare_echo},
    {"/cafe", "GET", ROUTE_EXACT, prepare_cafe},
    {"/menu", "GET", ROUTE_EXACT, prepare_menu},
    {"/assets/", "GET", ROUTE_PREFIX, prepare_assets},
};

static bool path_has_prefix(struct http_slice path, const char *prefix) {
    size_t length = strlen(prefix);
    return path.length >= length && memcmp(path.data, prefix, length) == 0;
}

static bool path_matches_rule(struct http_slice path, const struct route_rule *rule) {
    if (rule->match == ROUTE_PREFIX) return path_has_prefix(path, rule->path);
    return http_slice_equal(path, rule->path);
}

static int select_rule(const struct http_request *r,
                       const struct route_rule **selected, const char **allow) {
    *selected = NULL;
    *allow = NULL;
    bool get = http_slice_equal(r->method, "GET");
    bool post = http_slice_equal(r->method, "POST");
    if (!get && !post) return 501;

    const struct route_rule *best = NULL;
    size_t best_length = 0;
    bool allow_get = false, allow_post = false;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const struct route_rule *rule = &routes[i];
        if (!path_matches_rule(r->path, rule)) continue;

        if (strcmp(rule->method, "GET") == 0) allow_get = true;
        if (strcmp(rule->method, "POST") == 0) allow_post = true;
        size_t length = strlen(rule->path);
        if (http_slice_equal(r->method, rule->method) &&
            (!best || length > best_length ||
             (length == best_length && rule->match == ROUTE_EXACT &&
              best->match == ROUTE_PREFIX))) {
            best = rule;
            best_length = length;
        }
    }
    if (best) {
        *selected = best;
        return 0;
    }
    if (!allow_get && !allow_post) return 404;
    *allow = allow_get && allow_post ? "GET, POST" : allow_get ? "GET" : "POST";
    return 405;
}

int http_select_handler(const struct http_request *request,
                        router_handler *route_handler_func, const char **allow) {
    const struct route_rule *selected;
    int status = select_rule(request, &selected, allow);
    *route_handler_func = status ? NULL : selected->route_handler_func;
    return status;
}

static void prepare_hello(const struct http_request *request,
                          struct router_response *response) {
    (void)request;
    *response = (struct router_response){
        .status = 200, .content_type = "text/plain", .body = "Hello", .length = 5
    };
    prepare_memory_provider(response);
}

static void prepare_echo(const struct http_request *request,
                         struct router_response *response) {
    *response = (struct router_response){
        .status = 200, .content_type = "application/octet-stream",
        .body = request->body.data, .length = request->body.length
    };
    prepare_memory_provider(response);
}

void router_prepare_error(struct router_response *response, int status,
                          const char *allow) {
    const char *reason = http_reason(status);
    *response = (struct router_response){
        .status = status, .content_type = "text/plain", .body = reason,
        .length = strlen(reason), .allow = allow
    };
    prepare_memory_provider(response);
}

struct file_body {
    int fd;
    unsigned char buffer[16384];
};

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

static void file_body_cleanup(void *state) {
    struct file_body *file = state;
    close(file->fd);
    free(file);
}

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

static int path_error_status(int error) {
    switch (error) {
        case ENOENT: case ENOTDIR: case EACCES: case EPERM:
        case ELOOP: case ENAMETOOLONG:
            return 404;
        default:
            return 500;
    }
}

static bool inside_directory(const char *directory, const char *path) {
    size_t length = strlen(directory);
    if (length == 1 && directory[0] == '/') return path[0] == '/';
    return strncmp(directory, path, length) == 0 &&
           (path[length] == '/' || path[length] == '\0');
}

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
    if (!root) { status = path_error_status(errno); goto fail; }
    boundary = realpath(allowed_directory, NULL);
    if (!boundary) { status = path_error_status(errno); goto fail; }
    if (!inside_directory(root, boundary)) { status = 404; goto fail; }
    resolved = realpath(candidate, NULL);
    if (!resolved) { status = path_error_status(errno); goto fail; }
    if (!inside_directory(boundary, resolved)) {
        status = 404;
        goto fail;
    }
    /* O_NONBLOCK prevents opening a FIFO from blocking before fstat rejects it.
     * The path may still change between realpath and open (TOCTOU). */
    fd = open(resolved, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { status = path_error_status(errno); goto fail; }
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

static void prepare_cafe(const struct http_request *request,
                         struct router_response *response) {
    (void)request;
    prepare_file("public/pages/cafe.html", "public", response);
}

static void prepare_menu(const struct http_request *request,
                         struct router_response *response) {
    (void)request;
    prepare_file("public/pages/menu.html", "public", response);
}

static void prepare_assets(const struct http_request *request,
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
