#include "http.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* HTTP 필드 이름은 ASCII 대소문자를 구분하지 않는다.
 * locale에 따라 결과가 바뀌지 않게 ASCII 범위만 직접 변환한다. */
static unsigned char ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int equals_ignore_case(const unsigned char *value, size_t length,
                              const char *expected) {
    if (length != strlen(expected)) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        if (ascii_lower(value[i]) != ascii_lower((unsigned char)expected[i])) {
            return 0;
        }
    }
    return 1;
}

/* 메서드/헤더 이름에 쓸 수 있는 HTTP token 문자. 공백, 콜론, 제어 문자는 제외한다. */
static int is_token(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

/* C 문자열 검색 대신 길이 안에서 CRLF를 찾는다.
 * 입력의 NUL에 검색이 중단되거나 버퍼 끝을 넘어 읽지 않도록 한다. */
static const unsigned char *find_crlf(const unsigned char *start,
                                     const unsigned char *end) {
    for (const unsigned char *p = start; p < end; ++p) {
        if (*p == '\r' && end - p >= 2 && p[1] == '\n') {
            return p;
        }
    }
    return NULL;
}

/* Connection의 값은 쉼표로 구분된 token 목록이다.
 * 요청의 close 의사는 기록하되 이 단계의 서버는 keep-alive 요청도 항상 닫는다. */
static int parse_connection(const unsigned char *value, size_t length, int *close_requested) {
    size_t i = 0;
    while (i < length) {
        while (i < length && (value[i] == ' ' || value[i] == '\t')) {
            ++i;
        }
        size_t start = i;
        while (i < length && is_token(value[i])) {
            ++i;
        }
        if (i == start) {
            return 400;
        }
        if (equals_ignore_case(value + start, i - start, "close")) {
            *close_requested = 1;
        }
        while (i < length && (value[i] == ' ' || value[i] == '\t')) {
            ++i;
        }
        if (i == length) {
            return 0;
        }
        if (value[i++] != ',' || i == length) {
            return 400;
        }
    }
    return 400;
}

/* NUL 종료되지 않은 10진수 구간을 변환한다. 곱셈 전에 상한을 검사해서
 * size_t 오버플로우로 거대한 Content-Length가 작은 값으로 바뀌는 것을 막는다. */
static int parse_content_length(const unsigned char *value, size_t length, size_t *out) {
    size_t result = 0;
    if (length == 0) {
        return 400;
    }
    for (size_t i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return 400;
        }
        size_t digit = (size_t)(value[i] - '0');
        if (result > (SIZE_MAX - digit) / 10) {
            return 400;
        }
        result = result * 10 + digit;
    }
    *out = result;
    return 0;
}

/* 헤더 수신 → 이 함수 → 본문 수신 순서로 호출하는 순수 파서.
 * header에는 CRLFCRLF까지 넘긴다. recv가 함께 받은 본문은 여기에 넘기지 않는다.
 * 실패 원인을 HTTP 상태 코드로 반환해서 I/O 계층이 오류 응답을 만들 수 있게 한다. */
int parse_http_request(const void *header, size_t length, struct http_request *out) {
    if (out == NULL) {
        return 400;
    }
    memset(out, 0, sizeof(*out));
    if (length > REQ_HEADER_MAX_BYTES) {
        return 431;
    }
    if (header == NULL || length < 4) {
        return 400;
    }
    const unsigned char *start = header;
    const unsigned char *end = start + length;
    if (memcmp(end - 4, "\r\n\r\n", 4) != 0) {
        return 400;
    }

    /* 요청 라인은 METHOD SP TARGET SP VERSION CRLF 형식만 허용한다.
     * strtok 대신 경계를 직접 계산하여 빈 요소와 중복 공백을 놓치지 않는다. */
    const unsigned char *line_end = find_crlf(start, end);
    if (line_end == NULL) {
        return 400;
    }
    const unsigned char *space1 = memchr(start, ' ', (size_t)(line_end - start));
    if (space1 == NULL || space1 == start) {
        return 400;
    }
    for (const unsigned char *p = start; p < space1; ++p) {
        if (!is_token(*p)) {
            return 400;
        }
    }
    size_t method_length = (size_t)(space1 - start);
    out->method = method_length == 3 && memcmp(start, "GET", 3) == 0 ? HTTP_GET :
                  method_length == 4 && memcmp(start, "POST", 4) == 0 ? HTTP_POST : HTTP_OTHER;
    const unsigned char *target = space1 + 1;
    const unsigned char *space2 = memchr(target, ' ', (size_t)(line_end - target));
    if (space2 == NULL || space2 == target || *target != '/') {
        return 400;
    }
    size_t target_length = (size_t)(space2 - target);
    if (target_length > REQ_TARGET_MAX_BYTES) {
        return 414;
    }
    for (size_t i = 0; i < target_length; ++i) {
        /* 이 서버는 /path?query 형식만 받는다. fragment와 원시 공백/제어 문자는 거부.
         * URI의 비ASCII 입력은 percent encoding으로 전달하도록 한다. */
        if (target[i] <= 32 || target[i] >= 127 || target[i] == '#') {
            return 400;
        }
    }
    memcpy(out->target, target, target_length);
    out->target[target_length] = '\0';
    const unsigned char *version = space2 + 1;
    if (line_end - version != 8 || memcmp(version, "HTTP/", 5) != 0 ||
        version[5] < '0' || version[5] > '9' || version[6] != '.' ||
        version[7] < '0' || version[7] > '9') {
        return 400;
    }
    if (memcmp(version, "HTTP/1.1", 8) != 0 && memcmp(version, "HTTP/1.0", 8) != 0) {
        return 505;
    }
    out->http11 = version[7] == '1';

    int host_seen = 0;
    int transfer_encoding_seen = 0;
    int expect_seen = 0;
    const unsigned char *line = line_end + 2;
    while (line < end - 2) {
        line_end = find_crlf(line, end);
        if (line_end == NULL || line_end == line) {
            return 400;
        }
        const unsigned char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon == NULL || colon == line) {
            return 400;
        }
        for (const unsigned char *p = line; p < colon; ++p) {
            if (!is_token(*p)) {
                return 400; /* Host : x 같은 콜론 앞 공백과 접힌 헤더도 거부. */
            }
        }
        const unsigned char *value = colon + 1;
        const unsigned char *value_end = line_end;
        for (const unsigned char *p = value; p < value_end; ++p) {
            if ((*p < 32 && *p != '\t') || *p == 127) {
                return 400;
            }
        }
        /* OWS(optional whitespace)는 헤더 값 양끝의 SP/HTAB만 제거한다. */
        while (value < value_end && (*value == ' ' || *value == '\t')) {
            ++value;
        }
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
            --value_end;
        }
        size_t name_length = (size_t)(colon - line);
        size_t value_length = (size_t)(value_end - value);
        if (equals_ignore_case(line, name_length, "host")) {
            if (host_seen || value_length == 0 || value_length >= sizeof(out->host)) {
                return 400;
            }
            for (size_t i = 0; i < value_length; ++i) {
                if (value[i] <= 32 || value[i] >= 127 || strchr("/,?#@\\", value[i]) != NULL) {
                    return 400;
                }
            }
            memcpy(out->host, value, value_length);
            out->host[value_length] = '\0';
            host_seen = 1;
        } else if (equals_ignore_case(line, name_length, "content-length")) {
            /* 동일한 숫자가 반복돼도 이 구현에서는 거부한다. 길이 해석을 하나로 고정. */
            if (out->has_content_length) {
                return 400;
            }
            int status = parse_content_length(value, value_length, &out->content_length);
            if (status != 0) {
                return status;
            }
            out->has_content_length = 1;
        } else if (equals_ignore_case(line, name_length, "transfer-encoding")) {
            transfer_encoding_seen = 1;
        } else if (equals_ignore_case(line, name_length, "connection")) {
            int status = parse_connection(value, value_length, &out->connection_close);
            if (status != 0) {
                return status;
            }
        } else if (equals_ignore_case(line, name_length, "expect")) {
            expect_seen = 1;
        }
        line = line_end + 2;
    }
    if (line != end - 2 || (out->http11 && !host_seen)) {
        return 400;
    }
    /* 길이를 두 방식으로 해석하게 두지 않는다. 본문을 기다리기 전에 판단해야 한다. */
    if (transfer_encoding_seen && out->has_content_length) {
        return 400;
    }
    if (transfer_encoding_seen) {
        return 501;
    }
    if (expect_seen) {
        return 417; /* 100-continue를 기다리는 클라이언트와 교착하지 않도록 즉시 응답. */
    }
    if (out->content_length > REQ_BODY_MAX_BYTES) {
        return 413;
    }
    if (out->method == HTTP_OTHER) {
        return 501;
    }
    if (out->method == HTTP_POST && !out->has_content_length) {
        return 411;
    }
    return 0;
}

const char *http_status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 417: return "Expectation Failed";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Internal Server Error";
    }
}

void make_error_response(int status, struct http_response *out) {
    memset(out, 0, sizeof(*out));
    out->status = status;
    out->content_type = "text/plain; charset=utf-8";
    out->body = http_status_reason(status);
    out->body_length = strlen(out->body);
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = ascii_lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* /hello의 name 값만 URL 디코딩한다. 본문을 해석하는 범용 form 파서가 아니다.
 * 출력은 인사 문자열에 넣으므로 NUL/개행 등 제어 문자는 허용하지 않는다. */
static int decode_name(const char *value, size_t length, char *out) {
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == '%') {
            if (length - i < 3) return 400;
            int high = hex_digit((unsigned char)value[i + 1]);
            int low = hex_digit((unsigned char)value[i + 2]);
            if (high < 0 || low < 0) return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c < 32 || c == 127 || used == HELLO_NAME_MAX_BYTES) return 400;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return 0;
}

static int greeting_name(const char *query, char *name) {
    strcpy(name, "World");
    int seen = 0;
    while (query != NULL && *query != '\0') {
        const char *end = strchr(query, '&');
        if (end == NULL) end = query + strlen(query);
        const char *equal = memchr(query, '=', (size_t)(end - query));
        size_t key_length = (size_t)((equal != NULL ? equal : end) - query);
        if (key_length == 4 && memcmp(query, "name", 4) == 0) {
            if (seen || equal == NULL) return 400;
            int status = decode_name(equal + 1, (size_t)(end - equal - 1), name);
            if (status != 0) return status;
            seen = 1;
        }
        query = *end == '&' ? end + 1 : NULL;
    }
    return 0;
}

/* 완성된 요청만 받아 응답을 결정한다. 수신/전송을 하지 않으므로 단위 테스트에서
 * 네트워크 없이 GET/POST와 경로별 결과를 검증할 수 있다. */
void route_request(const struct http_request *request, struct http_response *out) {
    const char *query = strchr(request->target, '?');
    size_t path_length = query != NULL ? (size_t)(query - request->target) : strlen(request->target);
    int root = path_length == 1 && request->target[0] == '/';
    int hello = path_length == 6 && memcmp(request->target, "/hello", 6) == 0;
    int echo = path_length == 5 && memcmp(request->target, "/echo", 5) == 0;
    if (!root && !hello && !echo) {
        make_error_response(404, out);
        return;
    }
    if ((echo && request->method != HTTP_POST) || (!echo && request->method != HTTP_GET)) {
        make_error_response(405, out);
        out->allow = echo ? "POST" : "GET";
        return;
    }
    memset(out, 0, sizeof(*out));
    out->status = 200;
    out->content_type = "text/plain";
    if (root) {
        /* Iteration 1의 정상 루트 응답은 헤더와 본문까지 그대로 유지한다. */
        out->body = "Hello";
        out->body_length = 5;
    } else if (echo) {
        /* strlen 금지: 본문에 NUL/이미지/임의 바이트가 있어도 받은 길이만큼 보낸다. */
        out->content_type = "application/octet-stream";
        out->body = request->body;
        out->body_length = request->body_length;
    } else {
        char name[HELLO_NAME_MAX_BYTES + 1];
        int status = greeting_name(query != NULL ? query + 1 : NULL, name);
        if (status != 0) {
            make_error_response(status, out);
            return;
        }
        int length = snprintf(out->text, sizeof(out->text), "Hello, %s!\n", name);
        if (length < 0 || (size_t)length >= sizeof(out->text)) {
            make_error_response(500, out);
            return;
        }
        out->content_type = "text/plain; charset=utf-8";
        out->body = out->text;
        out->body_length = (size_t)length;
    }
}
