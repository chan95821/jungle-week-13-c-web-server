#define _XOPEN_SOURCE 700
#include "handlers.h"
#include "cgi_protocol.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
static bool is_token_char_cgi(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Find earliest CGI header terminator (\n\n or \r\n\r\n) and set sep_len, or SIZE_MAX. */
size_t cgi_find_header_end(const unsigned char *buf, size_t len, size_t *sep_len) {
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

/* Parse CGI Status field (200..599 excluding 204/304). Return false on invalid code. */
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

/* Parse CGI response headers; require Content-Type, accept optional Status, ignore framing headers. */
bool cgi_parse_headers(const unsigned char *hdr, size_t hdr_len,
                              char **content_type, int *status_out) {
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
            (*content_type) = malloc(vlen + 1);
            if (!(*content_type)) return false;
            memcpy((*content_type), val, vlen);
            (*content_type)[vlen] = '\0';
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

int cgi_resolve_script(struct http_slice path, char **resolved_out,
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
    if (!root) return router_path_error_status(errno);

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
        int st = router_path_error_status(errno);
        free(root);
        return st;
    }
    if (!router_inside_directory(root, resolved)) {
        free(root);
        free(resolved);
        return 404;
    }
    free(root);

    struct stat info;
    if (stat(resolved, &info) < 0) {
        int st = router_path_error_status(errno);
        free(resolved);
        return st;
    }
    if (!S_ISREG(info.st_mode) || access(resolved, X_OK) < 0) {
        int st = S_ISREG(info.st_mode) ? router_path_error_status(errno) : 404;
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

/* Allocate a "KEY=VALUE" environment string for execve(). Return NULL on OOM/overflow. */
char *cgi_make_env_var(const char *key, struct http_slice value) {
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

