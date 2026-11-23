#define _XOPEN_SOURCE 700
#include "router.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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
static void prepare_greet(const struct http_request *request,
                          struct router_response *response);
static void prepare_cgi(const struct http_request *request,
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
    {"/greet", "GET", ROUTE_EXACT, prepare_greet},
    {"/cgi-bin/", "GET", ROUTE_PREFIX, prepare_cgi},
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

/* --- Owned dynamic memory provider (GET /greet) --- */

struct owned_body {
    unsigned char *data;
    size_t length;
    size_t sent;
};

static enum body_result owned_body_next(void *state, struct http_slice *chunk) {
    struct owned_body *body = state;
    if (body->sent == body->length) return BODY_END;
    *chunk = (struct http_slice){body->data + body->sent, body->length - body->sent};
    body->sent = body->length;
    return BODY_DATA;
}

static void owned_body_cleanup(void *state) {
    struct owned_body *body = state;
    free(body->data);
    free(body);
}

static bool prepare_owned_memory_provider(struct router_response *response,
                                          int status, const char *content_type,
                                          unsigned char *data, size_t length) {
    struct owned_body *body = malloc(sizeof(*body));
    if (!body) {
        free(data);
        return false;
    }
    *body = (struct owned_body){.data = data, .length = length, .sent = 0};
    *response = (struct router_response){
        .status = status,
        .content_type = content_type,
        .length = length,
        .body_provider = {
            .next_func = owned_body_next,
            .cleanup_func = owned_body_cleanup,
            .state = body
        }
    };
    return true;
}

/* --- Dynamic query decoding and HTML greeting (GET /greet) --- */

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Returns 0 on success, 400 on invalid percent syntax or NUL byte, 500 on OOM. */
static int decode_form_slice(struct http_slice raw, unsigned char **out_data,
                             size_t *out_len) {
    *out_data = NULL;
    *out_len = 0;
    if (raw.length == 0) return 0;
    unsigned char *buf = malloc(raw.length);
    if (!buf) return 500;
    size_t out = 0;
    for (size_t i = 0; i < raw.length; ++i) {
        unsigned char c = raw.data[i];
        if (c == '+') {
            buf[out++] = ' ';
        } else if (c == '%') {
            if (i + 2 >= raw.length) {
                free(buf);
                return 400;
            }
            int hi = hex_value(raw.data[i + 1]);
            int lo = hex_value(raw.data[i + 2]);
            if (hi < 0 || lo < 0) {
                free(buf);
                return 400;
            }
            unsigned char decoded = (unsigned char)((hi << 4) | lo);
            if (decoded == 0) {
                free(buf);
                return 400;
            }
            buf[out++] = decoded;
            i += 2;
        } else {
            if (c == 0) {
                free(buf);
                return 400;
            }
            buf[out++] = c;
        }
    }
    *out_data = buf;
    *out_len = out;
    return 0;
}

static int extract_query_name(struct http_slice query, bool *found,
                              unsigned char **out_val, size_t *out_len) {
    *found = false;
    *out_val = NULL;
    *out_len = 0;
    size_t pos = 0;
    while (pos < query.length) {
        size_t seg_end = pos;
        while (seg_end < query.length && query.data[seg_end] != '&') ++seg_end;
        if (seg_end > pos) {
            size_t eq = pos;
            while (eq < seg_end && query.data[eq] != '=') ++eq;
            struct http_slice raw_key = {query.data + pos, eq - pos};
            struct http_slice raw_val = eq < seg_end
                ? (struct http_slice){query.data + eq + 1, seg_end - eq - 1}
                : (struct http_slice){NULL, 0};

            unsigned char *kbuf = NULL, *vbuf = NULL;
            size_t klen = 0, vlen = 0;
            int status = decode_form_slice(raw_key, &kbuf, &klen);
            if (status == 0) status = decode_form_slice(raw_val, &vbuf, &vlen);
            if (status != 0) {
                free(kbuf);
                free(vbuf);
                free(*out_val);
                *out_val = NULL;
                *out_len = 0;
                *found = false;
                return status;
            }
            if (!*found && klen == 4 && memcmp(kbuf, "name", 4) == 0) {
                *found = true;
                *out_val = vbuf;
                *out_len = vlen;
                vbuf = NULL;
            }
            free(kbuf);
            free(vbuf);
        }
        pos = seg_end + 1;
    }
    return 0;
}

static bool has_control_char(const unsigned char *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = data[i];
        if ((c >= 0x01 && c <= 0x1F) || c == 0x7F) return true;
    }
    return false;
}

static struct http_slice trim_ascii_spaces(const unsigned char *data, size_t length) {
    size_t start = 0;
    while (start < length && data[start] == ' ') ++start;
    size_t end = length;
    while (end > start && data[end - 1] == ' ') --end;
    return (struct http_slice){data + start, end - start};
}

static bool html_entity(unsigned char c, const char **entity, size_t *len) {
    const char *text = NULL;
    size_t elen = 0;
    switch (c) {
        case '&': text = "&amp;"; elen = 5; break;
        case '<': text = "&lt;"; elen = 4; break;
        case '>': text = "&gt;"; elen = 4; break;
        case '"': text = "&quot;"; elen = 6; break;
        case '\'': text = "&#39;"; elen = 5; break;
        default: return false;
    }
    if (entity) *entity = text;
    if (len) *len = elen;
    return true;
}

static bool html_escaped_length(struct http_slice text, size_t *out_len) {
    size_t total = 0;
    for (size_t i = 0; i < text.length; ++i) {
        size_t add = 1;
        html_entity(text.data[i], NULL, &add);
        if (total > SIZE_MAX - add) return false;
        total += add;
    }
    *out_len = total;
    return true;
}

static void append_html_escaped(unsigned char *dst, struct http_slice text) {
    size_t pos = 0;
    for (size_t i = 0; i < text.length; ++i) {
        const char *entity;
        size_t elen;
        if (html_entity(text.data[i], &entity, &elen)) {
            memcpy(dst + pos, entity, elen);
            pos += elen;
        } else {
            dst[pos++] = text.data[i];
        }
    }
}

static bool build_greet_html(struct http_slice display_name,
                             unsigned char **out_html, size_t *out_len) {
    static const char prefix[] =
        "<!doctype html>\n"
        "<html lang=\"ko\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <title>환영합니다 | 정글 카페</title>\n"
        "  <link rel=\"stylesheet\" href=\"/assets/cafe.css\">\n"
        "</head>\n"
        "<body>\n"
        "  <main>\n"
        "    <p class=\"eyebrow\">JUNGLE CAFE</p>\n"
        "    <section id=\"greet-result\" class=\"result-card\">\n"
        "      <h1>어서 오세요, ";
    static const char suffix[] =
        "님!</h1>\n"
        "      <p>정글 카페에서 따뜻한 커피와 함께 여유로운 시간 보내세요.</p>\n"
        "    </section>\n"
        "    <p class=\"nav-links\"><a href=\"/cafe\">← 카페 소개</a> · "
        "<a href=\"/menu\">메뉴 보기 →</a></p>\n"
        "  </main>\n"
        "</body>\n"
        "</html>\n";

    size_t escaped_len = 0;
    if (!html_escaped_length(display_name, &escaped_len)) return false;
    size_t prefix_len = sizeof(prefix) - 1;
    size_t suffix_len = sizeof(suffix) - 1;
    if (escaped_len > SIZE_MAX - prefix_len ||
        prefix_len + escaped_len > SIZE_MAX - suffix_len) {
        return false;
    }
    size_t total = prefix_len + escaped_len + suffix_len;
    unsigned char *html = malloc(total);
    if (!html) return false;
    memcpy(html, prefix, prefix_len);
    append_html_escaped(html + prefix_len, display_name);
    memcpy(html + prefix_len + escaped_len, suffix, suffix_len);
    *out_html = html;
    *out_len = total;
    return true;
}

static void prepare_greet(const struct http_request *request,
                          struct router_response *response) {
    bool found = false;
    unsigned char *name_buf = NULL;
    size_t name_len = 0;
    int status = extract_query_name(request->query, &found, &name_buf, &name_len);
    if (status != 0) {
        router_prepare_error(response, status, NULL);
        return;
    }
    if (found && has_control_char(name_buf, name_len)) {
        free(name_buf);
        router_prepare_error(response, 400, NULL);
        return;
    }
    struct http_slice display = found
        ? trim_ascii_spaces(name_buf, name_len)
        : (struct http_slice){NULL, 0};
    if (display.length == 0) {
        static const unsigned char fallback[] = "손님";
        display = (struct http_slice){fallback, sizeof(fallback) - 1};
    }
    unsigned char *html = NULL;
    size_t html_len = 0;
    if (!build_greet_html(display, &html, &html_len)) {
        free(name_buf);
        router_prepare_error(response, 500, NULL);
        return;
    }
    free(name_buf);
    if (!prepare_owned_memory_provider(response, 200, "text/html; charset=utf-8",
                                       html, html_len)) {
        router_prepare_error(response, 500, NULL);
    }
}

/* --- CGI execution and chunked streaming (GET /cgi-bin/...) --- */

#define CGI_HEADER_MAX_BYTES 8192
#define CGI_READ_TIMEOUT_MS 5000
#define CGI_CHUNK_BUFFER_SIZE 16384

struct cgi_body {
    int fd;
    pid_t pid;
    bool reaped;
    bool eof_reached;
    char *content_type;
    size_t initial_pos;
    size_t initial_len;
    unsigned char buffer[CGI_CHUNK_BUFFER_SIZE];
};

static bool make_deadline(struct timespec *deadline, int timeout_ms) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) return false;
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return true;
}

static int wait_readable(int fd, const struct timespec *deadline) {
    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        long sec = (long)(deadline->tv_sec - now.tv_sec);
        long nsec = deadline->tv_nsec - now.tv_nsec;
        long long remaining_ms = (long long)sec * 1000LL + (nsec + 999999L) / 1000000L;
        if (remaining_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        int timeout = remaining_ms > INT32_MAX ? INT32_MAX : (int)remaining_ms;
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        return 0;
    }
}

static bool is_token_char_cgi(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static size_t find_header_end(const unsigned char *buf, size_t len, size_t *sep_len) {
    for (size_t i = 0; i < len; ++i) {
        if (i + 2 <= len && buf[i] == '\n' && buf[i + 1] == '\n') {
            *sep_len = 2;
            return i;
        }
        if (i + 4 <= len && buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *sep_len = 4;
            return i;
        }
    }
    return SIZE_MAX;
}

static bool parse_cgi_status(const char *val, size_t len, int *status_out) {
    if (len < 3) return false;
    for (size_t i = 0; i < 3; ++i) {
        if (val[i] < '0' || val[i] > '9') return false;
    }
    if (len > 3 && val[3] != ' ' && val[3] != '\t') return false;
    int code = (val[0] - '0') * 100 + (val[1] - '0') * 10 + (val[2] - '0');
    if (code < 200 || code > 599 || code == 204 || code == 304) return false;
    *status_out = code;
    return true;
}

static bool header_name_equal(const char *name, size_t len, const char *target) {
    return len == strlen(target) && strncasecmp(name, target, len) == 0;
}

static bool parse_cgi_headers(const unsigned char *hdr, size_t hdr_len,
                              struct cgi_body *cgi, int *status_out) {
    int status = 200;
    bool seen_type = false, seen_status = false;
    size_t pos = 0;
    while (pos < hdr_len) {
        size_t line_end = pos;
        while (line_end < hdr_len && hdr[line_end] != '\n') ++line_end;
        size_t content_end = line_end;
        if (content_end > pos && hdr[content_end - 1] == '\r') --content_end;
        if (content_end == pos) return false;

        const unsigned char *line = hdr + pos;
        size_t len = content_end - pos;
        size_t colon = 0;
        while (colon < len && line[colon] != ':') {
            if (!is_token_char_cgi(line[colon])) return false;
            ++colon;
        }
        if (colon == 0 || colon == len) return false;

        size_t vstart = colon + 1;
        while (vstart < len && (line[vstart] == ' ' || line[vstart] == '\t')) ++vstart;
        size_t vend = len;
        while (vend > vstart && (line[vend - 1] == ' ' || line[vend - 1] == '\t')) --vend;
        for (size_t i = vstart; i < vend; ++i) {
            unsigned char c = line[i];
            if ((c < 32 && c != '\t') || c == 127) return false;
        }

        const char *name = (const char *)line;
        const char *val = (const char *)(line + vstart);
        size_t vlen = vend - vstart;
        if (header_name_equal(name, colon, "content-type")) {
            if (seen_type || vlen == 0) return false;
            seen_type = true;
            cgi->content_type = malloc(vlen + 1);
            if (!cgi->content_type) return false;
            memcpy(cgi->content_type, val, vlen);
            cgi->content_type[vlen] = '\0';
        } else if (header_name_equal(name, colon, "status")) {
            if (seen_status || !parse_cgi_status(val, vlen, &status)) return false;
            seen_status = true;
        }
        pos = line_end < hdr_len ? line_end + 1 : hdr_len;
    }
    if (!seen_type) return false;
    *status_out = status;
    return true;
}

static int reap_cgi_child(struct cgi_body *cgi, bool force_terminate) {
    if (cgi->reaped || cgi->pid <= 0) return -1;
    int wstatus = 0;
    if (!force_terminate) {
        pid_t res;
        do {
            res = waitpid(cgi->pid, &wstatus, 0);
        } while (res < 0 && errno == EINTR);
        if (res < 0) return -1;
        cgi->reaped = true;
        return wstatus;
    }
    pid_t res = waitpid(cgi->pid, &wstatus, WNOHANG);
    if (res == 0) {
        kill(cgi->pid, SIGTERM);
        for (int attempt = 0; attempt < 20; ++attempt) {
            struct timespec pause_ts = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&pause_ts, NULL);
            res = waitpid(cgi->pid, &wstatus, WNOHANG);
            if (res != 0) break;
        }
        if (res == 0) {
            kill(cgi->pid, SIGKILL);
            do {
                res = waitpid(cgi->pid, &wstatus, 0);
            } while (res < 0 && errno == EINTR);
        }
    } else if (res < 0 && errno == EINTR) {
        do {
            res = waitpid(cgi->pid, &wstatus, 0);
        } while (res < 0 && errno == EINTR);
    }
    if (res > 0) cgi->reaped = true;
    return wstatus;
}

static enum body_result cgi_body_next(void *state, struct http_slice *chunk) {
    struct cgi_body *cgi = state;
    if (cgi->initial_pos < cgi->initial_len) {
        *chunk = (struct http_slice){
            cgi->buffer + cgi->initial_pos,
            cgi->initial_len - cgi->initial_pos
        };
        cgi->initial_pos = cgi->initial_len;
        return BODY_DATA;
    }
    if (cgi->eof_reached) return BODY_END;

    struct timespec deadline;
    if (!make_deadline(&deadline, CGI_READ_TIMEOUT_MS)) return BODY_ERROR;
    if (wait_readable(cgi->fd, &deadline) < 0) return BODY_ERROR;

    ssize_t count;
    do {
        count = read(cgi->fd, cgi->buffer, sizeof(cgi->buffer));
    } while (count < 0 && errno == EINTR);
    if (count < 0) return BODY_ERROR;
    if (count > 0) {
        *chunk = (struct http_slice){cgi->buffer, (size_t)count};
        return BODY_DATA;
    }

    int wstatus = reap_cgi_child(cgi, false);
    if (wstatus < 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        errno = EIO;
        return BODY_ERROR;
    }
    cgi->eof_reached = true;
    return BODY_END;
}

static void cgi_body_cleanup(void *state) {
    struct cgi_body *cgi = state;
    if (cgi->fd >= 0) {
        close(cgi->fd);
        cgi->fd = -1;
    }
    if (!cgi->reaped && cgi->pid > 0) reap_cgi_child(cgi, true);
    free(cgi->content_type);
    free(cgi);
}

static int resolve_cgi_script(struct http_slice path, char **resolved_out,
                              char **script_name_out) {
    static const char prefix[] = "/cgi-bin/";
    size_t prefix_len = sizeof(prefix) - 1;
    *resolved_out = NULL;
    *script_name_out = NULL;

    if (memchr(path.data, '%', path.length)) return 400;
    if (path.length <= prefix_len) return 404;
    const unsigned char *rel = path.data + prefix_len;
    size_t rel_len = path.length - prefix_len;
    if (memchr(rel, '/', rel_len)) return 404;
    if ((rel_len == 1 && rel[0] == '.') ||
        (rel_len == 2 && rel[0] == '.' && rel[1] == '.')) {
        return 404;
    }

    char *root = realpath("cgi-bin", NULL);
    if (!root) return path_error_status(errno);

    size_t cand_len = (sizeof("cgi-bin/") - 1) + rel_len;
    char *candidate = malloc(cand_len + 1);
    if (!candidate) {
        free(root);
        return 500;
    }
    memcpy(candidate, "cgi-bin/", sizeof("cgi-bin/") - 1);
    memcpy(candidate + sizeof("cgi-bin/") - 1, rel, rel_len);
    candidate[cand_len] = '\0';

    char *resolved = realpath(candidate, NULL);
    free(candidate);
    if (!resolved) {
        int st = path_error_status(errno);
        free(root);
        return st;
    }
    if (!inside_directory(root, resolved)) {
        free(root);
        free(resolved);
        return 404;
    }
    free(root);

    struct stat info;
    if (stat(resolved, &info) < 0) {
        int st = path_error_status(errno);
        free(resolved);
        return st;
    }
    if (!S_ISREG(info.st_mode) || access(resolved, X_OK) < 0) {
        int st = S_ISREG(info.st_mode) ? path_error_status(errno) : 404;
        free(resolved);
        return st;
    }

    char *script_name = malloc(path.length + 1);
    if (!script_name) {
        free(resolved);
        return 500;
    }
    memcpy(script_name, path.data, path.length);
    script_name[path.length] = '\0';
    *resolved_out = resolved;
    *script_name_out = script_name;
    return 0;
}

static char *make_env_var(const char *key, struct http_slice value) {
    size_t klen = strlen(key);
    if (value.length > SIZE_MAX - klen - 2) return NULL;
    char *entry = malloc(klen + 1 + value.length + 1);
    if (!entry) return NULL;
    memcpy(entry, key, klen);
    entry[klen] = '=';
    if (value.length > 0 && value.data) memcpy(entry + klen + 1, value.data, value.length);
    entry[klen + 1 + value.length] = '\0';
    return entry;
}

static int spawn_cgi_process(const char *resolved, const char *script_name,
                             struct http_slice query, pid_t *pid_out, int *fd_out) {
    *pid_out = -1;
    *fd_out = -1;

    char *exec_arg = malloc(strlen(resolved) + 1);
    char *env_method = make_env_var("REQUEST_METHOD", (struct http_slice){(const unsigned char *)"GET", 3});
    char *env_query = make_env_var("QUERY_STRING", query);
    char *env_script = make_env_var("SCRIPT_NAME", (struct http_slice){(const unsigned char *)script_name, strlen(script_name)});
    char *env_proto = make_env_var("SERVER_PROTOCOL", (struct http_slice){(const unsigned char *)"HTTP/1.1", 8});
    char *env_path = make_env_var("PATH", (struct http_slice){(const unsigned char *)"/usr/local/bin:/usr/bin:/bin", 28});
    if (!exec_arg || !env_method || !env_query || !env_script || !env_proto || !env_path) {
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }
    memcpy(exec_arg, resolved, strlen(resolved) + 1);
    char *argv[] = {exec_arg, NULL};
    char *envp[] = {env_method, env_query, env_script, env_proto, env_path, NULL};

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        close(pipefd[0]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0) _exit(127);
        if (devnull != STDIN_FILENO) {
            if (dup2(devnull, STDIN_FILENO) < 0) _exit(127);
            close(devnull);
        }
        if (pipefd[1] != STDOUT_FILENO) {
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
            close(pipefd[1]);
        }
        execve(resolved, argv, envp);
        _exit(127);
    }

    close(pipefd[1]);
    free(exec_arg); free(env_method); free(env_query);
    free(env_script); free(env_proto); free(env_path);
    *pid_out = pid;
    *fd_out = pipefd[0];
    return 0;
}

static int read_cgi_header(struct cgi_body *cgi, int *status_out) {
    unsigned char raw[CGI_HEADER_MAX_BYTES];
    size_t used = 0;
    struct timespec deadline;
    if (!make_deadline(&deadline, CGI_READ_TIMEOUT_MS)) return 500;

    for (;;) {
        size_t sep_len = 0;
        size_t end = find_header_end(raw, used, &sep_len);
        if (end != SIZE_MAX) {
            if (!parse_cgi_headers(raw, end, cgi, status_out)) return 502;
            size_t body_start = end + sep_len;
            size_t rem = used - body_start;
            if (rem > 0) {
                memcpy(cgi->buffer, raw + body_start, rem);
                cgi->initial_pos = 0;
                cgi->initial_len = rem;
            }
            return 0;
        }
        if (used == sizeof(raw)) return 502;
        if (wait_readable(cgi->fd, &deadline) < 0) return 502;
        ssize_t count;
        do {
            count = read(cgi->fd, raw + used, sizeof(raw) - used);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) return 502;
        used += (size_t)count;
    }
}

static void prepare_cgi(const struct http_request *request,
                        struct router_response *response) {
    char *resolved = NULL, *script_name = NULL;
    int err = resolve_cgi_script(request->path, &resolved, &script_name);
    if (err != 0) {
        router_prepare_error(response, err, NULL);
        return;
    }

    struct cgi_body *cgi = calloc(1, sizeof(*cgi));
    if (!cgi) {
        free(resolved);
        free(script_name);
        router_prepare_error(response, 500, NULL);
        return;
    }
    cgi->fd = -1;
    cgi->pid = -1;

    err = spawn_cgi_process(resolved, script_name, request->query, &cgi->pid, &cgi->fd);
    free(resolved);
    free(script_name);
    if (err != 0) {
        cgi_body_cleanup(cgi);
        router_prepare_error(response, err, NULL);
        return;
    }

    int http_status = 200;
    err = read_cgi_header(cgi, &http_status);
    if (err != 0) {
        cgi_body_cleanup(cgi);
        router_prepare_error(response, err, NULL);
        return;
    }

    *response = (struct router_response){
        .status = http_status,
        .content_type = cgi->content_type,
        .chunked = true,
        .body_provider = {
            .next_func = cgi_body_next,
            .cleanup_func = cgi_body_cleanup,
            .state = cgi
        }
    };
}
