#include "http.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

bool http_slice_equal(struct http_slice value, const char *text) {
    size_t length = strlen(text);
    return value.length == length &&
           (length == 0 || memcmp(value.data, text, length) == 0);
}

static unsigned char ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Compare slice against lowercase ASCII string case-insensitively. */
static bool equal_case(struct http_slice value, const char *text) {
    if (value.length != strlen(text)) return false;
    for (size_t i = 0; i < value.length; ++i) {
        if (ascii_lower(value.data[i]) != (unsigned char)text[i]) return false;
    }
    return true;
}

static bool alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool hex_digit(unsigned char c) {
    return digit(c) || (ascii_lower(c) >= 'a' && ascii_lower(c) <= 'f');
}
static bool unreserved(unsigned char c) {
    return alpha(c) || digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
static bool sub_delim(unsigned char c) {
    return c != 0 && strchr("!$&'()*+,;=", c) != NULL;
}
static bool token_char(unsigned char c) {
    return alpha(c) || digit(c) ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}
static bool ows(unsigned char c) { return c == ' ' || c == '\t'; }

/* Trim leading and trailing optional whitespace (SP/HTAB) from a slice. */
static struct http_slice trim(struct http_slice value) {
    while (value.length && ows(value.data[0])) {
        ++value.data;
        --value.length;
    }
    while (value.length && ows(value.data[value.length - 1])) --value.length;
    return value;
}

/* Return index of first CRLF in buffer, or SIZE_MAX if none exists. */
static size_t crlf(const unsigned char *data, size_t length) {
    for (size_t i = 0; i + 1 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    }
    return SIZE_MAX;
}

/* Validate RFC 3986 reg-name syntax (unreserved / sub-delims / pct-encoded). */
static bool valid_reg_name(struct http_slice name) {
    if (!name.length) return false;
    for (size_t i = 0; i < name.length; ++i) {
        unsigned char c = name.data[i];
        if (unreserved(c) || sub_delim(c)) continue;
        if (c != '%' || i + 2 >= name.length ||
            !hex_digit(name.data[i + 1]) || !hex_digit(name.data[i + 2])) {
            return false;
        }
        i += 2;
    }
    return true;
}

/* Validate RFC 3986 IP-literal (IPv6 via inet_pton or IPvFuture syntax). */
static bool valid_ip_literal(struct http_slice value) {
    if (value.length > 3 && ascii_lower(value.data[0]) == 'v') {
        size_t i = 1;
        while (i < value.length && hex_digit(value.data[i])) ++i;
        if (i == 1 || i + 1 >= value.length || value.data[i++] != '.') return false;
        for (; i < value.length; ++i) {
            unsigned char c = value.data[i];
            if (!unreserved(c) && !sub_delim(c) && c != ':') return false;
        }
        return true;
    }
    char address[INET6_ADDRSTRLEN];
    unsigned char binary[16];
    if (!value.length || value.length >= sizeof(address)) return false;
    memcpy(address, value.data, value.length);
    address[value.length] = '\0';
    return inet_pton(AF_INET6, address, binary) == 1;
}

/* Validate Host header syntax (reg-name or bracketed IP-literal + optional port). */
static bool valid_host(struct http_slice value) {
    if (!value.length) return false;

    size_t port_start;
    if (value.data[0] == '[') {
        size_t end = 1;
        while (end < value.length && value.data[end] != ']') ++end;
        if (end == value.length ||
            !valid_ip_literal((struct http_slice){value.data + 1, end - 1})) {
            return false;
        }
        port_start = end + 1;
    } else {
        port_start = 0;
        while (port_start < value.length && value.data[port_start] != ':') ++port_start;
        if (!valid_reg_name((struct http_slice){value.data, port_start})) return false;
    }

    if (port_start == value.length) return true;
    if (value.data[port_start++] != ':') return false;

    /* URI port syntax permits an empty port; no DNS lookup is performed. */
    unsigned int port = 0;
    for (size_t i = port_start; i < value.length; ++i) {
        if (!digit(value.data[i])) return false;
        unsigned int number = (unsigned int)(value.data[i] - '0');
        if (port > (65535U - number) / 10U) return false;
        port = port * 10U + number;
    }
    return true;
}

/* Check RFC 3986 URI characters and %XX syntax without inspecting decoded bytes. */
static bool uri_characters(struct http_slice value, bool origin) {
    for (size_t i = 0; i < value.length; ++i) {
        unsigned char c = value.data[i];
        if (c == '?') continue;
        if (unreserved(c) || sub_delim(c) || c == ':' || c == '@' || c == '/' ||
            (!origin && (c == '[' || c == ']'))) continue;
        if (c == '%' && i + 2 < value.length &&
            hex_digit(value.data[i + 1]) && hex_digit(value.data[i + 2])) {
            i += 2;
            continue;
        }
        return false;
    }
    return true;
}

/* Parse HTTP/1.1 request-line into method, target, path, and raw query slices. */
static int parse_start_line(struct http_slice line, struct http_request *r,
                            bool *unsupported_target) {
    size_t first = 0;
    while (first < line.length && line.data[first] != ' ') {
        if (!token_char(line.data[first])) return 400;
        ++first;
    }
    if (!first || first == line.length) return 400;

    size_t second = first + 1;
    while (second < line.length && line.data[second] != ' ') ++second;
    if (second == first + 1 || second == line.length) return 400;

    r->method = (struct http_slice){line.data, first};
    r->target = (struct http_slice){line.data + first + 1, second - first - 1};
    r->version = (struct http_slice){line.data + second + 1, line.length - second - 1};

    if (!http_slice_equal(r->version, "HTTP/1.1")) {
        if (r->version.length == 8 && memcmp(r->version.data, "HTTP/", 5) == 0 &&
            digit(r->version.data[5]) && r->version.data[6] == '.' &&
            digit(r->version.data[7])) return 505;
        return 400;
    }

    bool origin = r->target.data[0] == '/';
    if (!uri_characters(r->target, origin)) return 400;

    if (!origin) {
        size_t prefix = 0;
        if (r->target.length >= 7 && equal_case((struct http_slice){r->target.data, 7}, "http://")) prefix = 7;
        if (r->target.length >= 8 && equal_case((struct http_slice){r->target.data, 8}, "https://")) prefix = 8;
        if (prefix) {
            size_t end = prefix;
            while (end < r->target.length && r->target.data[end] != '/' &&
                   r->target.data[end] != '?') ++end;
            if (!valid_host((struct http_slice){r->target.data + prefix, end - prefix})) return 400;
        } else if (!(http_slice_equal(r->method, "OPTIONS") &&
                     http_slice_equal(r->target, "*")) &&
                   !(http_slice_equal(r->method, "CONNECT") && valid_host(r->target))) {
            return 400;
        }
        *unsupported_target = true;
        return 0;
    }

    size_t path_length = 0;
    while (path_length < r->target.length && r->target.data[path_length] != '?') ++path_length;
    r->path = (struct http_slice){r->target.data, path_length};

    if (path_length < r->target.length) {
        r->query = (struct http_slice){
            r->target.data + path_length + 1,
            r->target.length - path_length - 1
        };
    } else {
        r->query = (struct http_slice){NULL, 0};
    }

    return 0;
}

/* Parse non-negative decimal Content-Length without size_t overflow. Return 0 or 400. */
static int parse_length(struct http_slice value, size_t *length) {
    if (!value.length) return 400;
    size_t result = 0;
    for (size_t i = 0; i < value.length; ++i) {
        if (!digit(value.data[i])) return 400;
        size_t number = (size_t)(value.data[i] - '0');
        if (result > (SIZE_MAX - number) / 10) return 400;
        result = result * 10 + number;
    }
    *length = result;
    return 0;
}

/* Parse Connection header option list for close/keep-alive tokens. Return 0 or 400. */
static int parse_connection(struct http_slice value, struct http_request *r) {
    size_t i = 0;
    do {
        while (i < value.length && ows(value.data[i])) ++i;
        size_t start = i;
        while (i < value.length && token_char(value.data[i])) ++i;
        if (i == start) return 400;
        struct http_slice token = {value.data + start, i - start};
        if (equal_case(token, "close")) r->connection_close = true;
        if (equal_case(token, "keep-alive")) r->connection_keep_alive = true;
        while (i < value.length && ows(value.data[i])) ++i;
        if (i == value.length) return 0;
        if (value.data[i++] != ',') return 400;
    } while (i < value.length);
    return 400;
}

/* Accept only 100-continue in Expect header; return 417 otherwise. */
static int parse_expect(struct http_slice value) {
    size_t i = 0;
    do {
        size_t start = i;
        while (i < value.length && value.data[i] != ',') ++i;
        if (!equal_case(trim((struct http_slice){value.data + start, i - start}),
                        "100-continue")) return 417;
        if (i == value.length) return 0;
        ++i;
    } while (i < value.length);
    return 417;
}

/* Validate the coding list even though no transfer coding is supported here. */
static int parse_transfer(struct http_slice value, bool *last_chunked,
                          unsigned int *chunked_count) {
    size_t i = 0;
    do {
        while (i < value.length && ows(value.data[i])) ++i;
        size_t start = i;
        while (i < value.length && token_char(value.data[i])) ++i;
        if (i == start) return 400;
        bool chunked = equal_case((struct http_slice){value.data + start, i - start}, "chunked");
        if (chunked && ++*chunked_count > 1) return 400;
        *last_chunked = chunked;
        while (i < value.length && ows(value.data[i])) ++i;
        while (i < value.length && value.data[i] == ';') {
            if (chunked) return 400;
            ++i;
            while (i < value.length && ows(value.data[i])) ++i;
            start = i;
            while (i < value.length && token_char(value.data[i])) ++i;
            if (i == start) return 400;
            while (i < value.length && ows(value.data[i])) ++i;
            if (i == value.length || value.data[i++] != '=') return 400;
            while (i < value.length && ows(value.data[i])) ++i;
            if (i < value.length && value.data[i] == '"') {
                ++i;
                bool closed = false;
                while (i < value.length) {
                    if (value.data[i] == '"') { ++i; closed = true; break; }
                    if (value.data[i] == '\\') {
                        if (++i == value.length) return 400;
                    }
                    unsigned char c = value.data[i++];
                    if ((c < 32 && c != '\t') || c == 127) return 400;
                }
                if (!closed) return 400;
            } else {
                start = i;
                while (i < value.length && token_char(value.data[i])) ++i;
                if (i == start) return 400;
            }
            while (i < value.length && ows(value.data[i])) ++i;
        }
        if (i == value.length) return 0;
        if (value.data[i++] != ',') return 400;
    } while (i < value.length);
    return 400;
}

int http_parse_request(const unsigned char *data, size_t length,
                       struct http_request *r) {
    if (!data || !r || length < 4) return 400;
    if (length > REQ_HEADER_MAX_BYTES) return 431;
    *r = (struct http_request){0};
    if (memcmp(data + length - 4, "\r\n\r\n", 4) != 0) return 400;

    size_t line_length = crlf(data, length);
    if (line_length == SIZE_MAX) return 400;

    bool unsupported_target = false;
    int status = parse_start_line((struct http_slice){data, line_length}, r,
                                  &unsupported_target);
    if (status) return status;

    r->headers = (struct http_slice){data + line_length + 2, length - line_length - 4};
    size_t offset = line_length + 2;
    bool host_seen = false, transfer_seen = false, last_chunked = false;
    unsigned int chunked_count = 0;
    int expect_status = 0;

    while (offset < length) {
        size_t end = crlf(data + offset, length - offset);
        if (end == SIZE_MAX) return 400;
        if (end == 0) {
            if (offset + 2 != length) return 400;
            break;
        }

        const unsigned char *line = data + offset;
        size_t colon = 0;
        while (colon < end && line[colon] != ':') {
            if (!token_char(line[colon])) return 400;
            ++colon;
        }
        if (!colon || colon == end) return 400;

        struct http_slice name = {line, colon};
        struct http_slice value = trim((struct http_slice){line + colon + 1, end - colon - 1});
        for (size_t i = 0; i < value.length; ++i) {
            unsigned char c = value.data[i];
            if ((c < 32 && c != '\t') || c == 127) return 400;
        }

        if (equal_case(name, "host")) {
            if (host_seen || !valid_host(value)) return 400;
            host_seen = true;
            r->host = value;
        } else if (equal_case(name, "content-length")) {
            if (r->has_content_length) return 400;
            r->has_content_length = true;
            status = parse_length(value, &r->content_length);
            if (status) return status;
        } else if (equal_case(name, "transfer-encoding")) {
            transfer_seen = true;
            status = parse_transfer(value, &last_chunked, &chunked_count);
            if (status) return status;
        } else if (equal_case(name, "connection")) {
            status = parse_connection(value, r);
            if (status) return status;
        } else if (equal_case(name, "expect")) {
            expect_status = parse_expect(value) ? 417 : expect_status;
            r->expect_continue = true;
        }

        offset += end + 2;
    }

    if (!host_seen) return 400;
    if (transfer_seen && r->has_content_length) return 400;
    if (transfer_seen) return last_chunked ? 501 : 400;
    if (r->content_length > REQ_BODY_MAX_BYTES) return 413;
    if (unsupported_target) return 501;
    if (expect_status) return expect_status;
    return 0;
}

const char *http_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}

bool http_field_name_equal(struct http_slice name, const char *lowercase_name) {
    return equal_case(name, lowercase_name);
}

int http_next_field(struct http_slice fields, size_t *offset, struct http_field *field) {
    if (*offset == fields.length) return 0;
    if (*offset > fields.length) return -1;
    const unsigned char *line = fields.data + *offset;
    size_t length = crlf(line, fields.length - *offset);
    if (length == SIZE_MAX || length == 0) return -1;
    size_t colon = 0;
    while (colon < length && line[colon] != ':') {
        if (!token_char(line[colon])) return -1;
        ++colon;
    }
    if (colon == 0 || colon == length) return -1;
    struct http_slice value = {line + colon + 1, length - colon - 1};
    for (size_t i = 0; i < value.length; ++i) {
        unsigned char c = value.data[i];
        if ((c < 32 && c != '	') || c == 127) return -1;
    }
    field->name = (struct http_slice){line, colon};
    field->value = trim(value);
    *offset += length + 2;
    return 1;
}
