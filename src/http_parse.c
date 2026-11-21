#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

static int is_tchar(unsigned char c) {
    if (isalnum(c)) {
        return 1;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return 1;
    default:
        return 0;
    }
}

static int is_vchar(unsigned char c) {
    return (c >= 0x21 && c <= 0x7E);
}

static int is_field_value_char(unsigned char c) {
    return is_vchar(c) || c == ' ' || c == '\t';
}

static int header_name_eq(const char *name, size_t name_len, const char *lit) {
    size_t lit_len = strlen(lit);
    return (name_len == lit_len) && (strncasecmp(name, lit, lit_len) == 0);
}

static const char *find_crlf(const char *buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

static int has_cr_or_lf(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            return 1;
        }
    }
    return 0;
}

int parse_request_line(const char *line, size_t line_len,
                       HttpRequest *req, HttpStatus *http_status) {
    if (line == NULL || req == NULL || http_status == NULL) {
        return -1;
    }
    if (line_len == 0 || has_cr_or_lf(line, line_len)) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }

    const char *sp1 = memchr(line, ' ', line_len);
    if (sp1 == NULL || sp1 == line) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }
    size_t method_len = (size_t)(sp1 - line);

    const char *uri_start = sp1 + 1;
    size_t rem_after_sp1 = line_len - method_len - 1;
    const char *sp2 = memchr(uri_start, ' ', rem_after_sp1);
    if (sp2 == NULL || sp2 == uri_start) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }
    size_t uri_len = (size_t)(sp2 - uri_start);

    const char *ver_start = sp2 + 1;
    size_t ver_len = line_len - method_len - 1 - uri_len - 1;
    if (ver_len == 0 || memchr(ver_start, ' ', ver_len) != NULL) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }

    for (size_t i = 0; i < method_len; i++) {
        if (!is_tchar((unsigned char)line[i])) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
    }
    int is_get = (method_len == 3 && memcmp(line, "GET", 3) == 0);
    int is_post = (method_len == 4 && memcmp(line, "POST", 4) == 0);
    if (!is_get && !is_post) {
        *http_status = HTTP_STATUS_METHOD_NOT_ALLOWED;
        return -1;
    }

    if (uri_start[0] != '/') {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }
    for (size_t i = 0; i < uri_len; i++) {
        if (!is_vchar((unsigned char)uri_start[i])) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
    }
    if (uri_len > REQ_URI_MAX_BYTES) {
        *http_status = HTTP_STATUS_URI_TOO_LONG;
        return -1;
    }

    if (ver_len == 8 && (memcmp(ver_start, "HTTP/1.1", 8) == 0 ||
                         memcmp(ver_start, "HTTP/1.0", 8) == 0)) {
        /* ok */
    } else if (ver_len >= 5 && memcmp(ver_start, "HTTP/", 5) == 0) {
        if (ver_len == 8 && isdigit((unsigned char)ver_start[5]) &&
            ver_start[6] == '.' && isdigit((unsigned char)ver_start[7])) {
            *http_status = HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;
        } else {
            *http_status = HTTP_STATUS_BAD_REQUEST;
        }
        return -1;
    } else {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }

    memcpy(req->method, line, method_len);
    req->method[method_len] = '\0';
    memcpy(req->uri, uri_start, uri_len);
    req->uri[uri_len] = '\0';
    memcpy(req->version, ver_start, ver_len);
    req->version[ver_len] = '\0';
    return 0;
}

int parse_header_line(const char *line, size_t line_len,
                      const char **out_name, size_t *out_name_len,
                      const char **out_val, size_t *out_val_len) {
    if (line == NULL || out_name == NULL || out_name_len == NULL ||
        out_val == NULL || out_val_len == NULL) {
        return -1;
    }
    if (line_len == 0 || has_cr_or_lf(line, line_len)) {
        return -1;
    }

    const char *colon = memchr(line, ':', line_len);
    if (colon == NULL || colon == line) {
        return -1;
    }

    size_t name_len = (size_t)(colon - line);
    for (size_t i = 0; i < name_len; i++) {
        if (!is_tchar((unsigned char)line[i])) {
            return -1;
        }
    }

    const char *val_start = colon + 1;
    size_t val_len = line_len - name_len - 1;
    while (val_len > 0 && (val_start[0] == ' ' || val_start[0] == '\t')) {
        val_start++;
        val_len--;
    }
    while (val_len > 0 && (val_start[val_len - 1] == ' ' || val_start[val_len - 1] == '\t')) {
        val_len--;
    }

    for (size_t i = 0; i < val_len; i++) {
        if (!is_field_value_char((unsigned char)val_start[i])) {
            return -1;
        }
    }

    *out_name = line;
    *out_name_len = name_len;
    *out_val = val_start;
    *out_val_len = val_len;
    return 0;
}

/* Content-Length 값의 순수 숫자·오버플로우(400)와 본문 상한(413)을 구분해 파싱한다. */
static int parse_content_length(const char *val, size_t val_len,
                                size_t *out_len, HttpStatus *http_status) {
    if (val_len == 0) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }

    size_t acc = 0;
    for (size_t i = 0; i < val_len; i++) {
        unsigned char c = (unsigned char)val[i];
        if (c < '0' || c > '9') {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
        size_t d = (size_t)(c - '0');
        if (acc > (SIZE_MAX - d) / 10) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
        acc = acc * 10 + d;
    }

    if (acc > REQ_BODY_MAX_BYTES) {
        *http_status = HTTP_STATUS_PAYLOAD_TOO_LARGE;
        return -1;
    }

    *out_len = acc;
    return 0;
}

/* 분리된 (name, val) 쌍의 프로토콜 의미를 해석해 req를 갱신한다. (유효한 미지원 헤더는 통과) */
static int apply_header_field(HttpRequest *req,
                              const char *name, size_t name_len,
                              const char *val, size_t val_len,
                              HttpStatus *http_status) {
    if (header_name_eq(name, name_len, "Host")) {
        if (req->has_host || val_len == 0 || val_len >= sizeof(req->host)) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
        memcpy(req->host, val, val_len);
        req->host[val_len] = '\0';
        req->has_host = 1;
        return 0;
    }

    if (header_name_eq(name, name_len, "Content-Length")) {
        if (req->has_content_length) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
        if (parse_content_length(val, val_len, &req->content_length, http_status) < 0) {
            return -1;
        }
        req->has_content_length = 1;
        return 0;
    }

    if (header_name_eq(name, name_len, "Transfer-Encoding")) {
        if (val_len == 0) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
        req->is_chunked = 1;
        return 0;
    }

    if (header_name_eq(name, name_len, "Content-Type")) {
        if (req->has_content_type || val_len == 0 || val_len >= sizeof(req->content_type)) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
        memcpy(req->content_type, val, val_len);
        req->content_type[val_len] = '\0';
        req->has_content_type = 1;
        return 0;
    }

    if (header_name_eq(name, name_len, "Connection")) {
        if (val_len == 5 && strncasecmp(val, "close", 5) == 0) {
            req->connection_close = 1;
        }
        return 0;
    }

    return 0;
}

int parse_headers(const char *headers, size_t headers_len,
                  HttpRequest *req, HttpStatus *http_status) {
    if (req == NULL || http_status == NULL) {
        return -1;
    }

    size_t pos = 0;
    while (pos < headers_len) {
        const char *crlf = find_crlf(headers + pos, headers_len - pos);
        if (crlf == NULL) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }

        size_t line_len = (size_t)(crlf - (headers + pos));
        const char *name = NULL;
        const char *val = NULL;
        size_t name_len = 0;
        size_t val_len = 0;

        if (parse_header_line(headers + pos, line_len,
                              &name, &name_len, &val, &val_len) < 0) {
            *http_status = HTTP_STATUS_BAD_REQUEST;
            return -1;
        }
        if (apply_header_field(req, name, name_len, val, val_len, http_status) < 0) {
            return -1;
        }

        pos += line_len + 2;
    }

    if (strcmp(req->version, "HTTP/1.1") == 0 && !req->has_host) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }
    if (req->is_chunked && req->has_content_length) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }
    if (req->is_chunked) {
        *http_status = HTTP_STATUS_NOT_IMPLEMENTED;
        return -1;
    }

    return 0;
}

/* 헤드 전체에서 제어 문자(0x00..0x1F, 0x7F)·단독 CR/LF·비-ASCII(0x80..0xFF)를 1차 차단한다. */
static int validate_head_bytes(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r') {
            if (i + 1 >= len || buf[i + 1] != '\n') {
                return -1;
            }
            i++;
            continue;
        }
        if (c == '\n') {
            return -1;
        }
        if (c == '\t' || c == ' ' || is_vchar(c)) {
            continue;
        }
        return -1;
    }
    return 0;
}

int parse_request_head(const char *buf, size_t header_len,
                       HttpRequest *req, HttpStatus *http_status) {
    if (buf == NULL || req == NULL || http_status == NULL) {
        return -1;
    }
    if (header_len < 4 || validate_head_bytes(buf, header_len) < 0) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }

    const char *first_crlf = find_crlf(buf, header_len);
    if (first_crlf == NULL || first_crlf == buf) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }

    size_t line_len = (size_t)(first_crlf - buf);
    if (parse_request_line(buf, line_len, req, http_status) < 0) {
        return -1;
    }

    size_t headers_offset = line_len + 2;
    if (headers_offset > header_len - 2) {
        *http_status = HTTP_STATUS_BAD_REQUEST;
        return -1;
    }
    size_t headers_len = (header_len - 2) - headers_offset;
    return parse_headers(buf + headers_offset, headers_len, req, http_status);
}
