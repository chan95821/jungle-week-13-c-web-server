#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct header_field {
    struct http_slice line;
    struct http_slice name;
    struct http_slice value;
};

static bool header_name_matches(struct http_slice name, const char *text) {
    return name.length == strlen(text) &&
           strncasecmp((const char *)name.data, text, name.length) == 0;
}

bool is_http_token_character(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') ||
           (byte != 0 && strchr("!#$%&'*+-.^_`|~", byte) != NULL);
}

static struct http_slice trim_header_value_whitespace(struct http_slice value) {
    while (value.length > 0 && (value.data[0] == ' ' || value.data[0] == '\t')) {
        ++value.data;
        --value.length;
    }
    while (value.length > 0 &&
           (value.data[value.length - 1] == ' ' || value.data[value.length - 1] == '\t')) {
        --value.length;
    }
    return value;
}

static size_t find_header_line_end(const unsigned char *bytes, size_t length) {
    for (size_t index = 0; index + 1 < length; ++index) {
        if (bytes[index] == '\r' && bytes[index + 1] == '\n') return index;
    }
    return SIZE_MAX;
}

/* slice는 원본을 빌린다. 이름·값·원본 줄을 함께 보관해 재인코딩 없이
 * 필드를 복사하며, 중복 Set-Cookie 등도 순서를 유지한다. */
static bool parse_next_header_field(const unsigned char *header, size_t length,
                               size_t *offset, struct header_field *field) {
    if (*offset >= length) return false;
    size_t line_length = find_header_line_end(header + *offset, length - *offset);
    if (line_length == SIZE_MAX || line_length == 0) return false;

    const unsigned char *line = header + *offset;
    size_t colon = 0;
    while (colon < line_length && is_http_token_character(line[colon])) ++colon;
    if (colon == 0 || colon == line_length || line[colon] != ':') return false;

    for (size_t index = colon + 1; index < line_length; ++index) {
        if ((line[index] < 32 && line[index] != '\t') || line[index] == 127) return false;
    }

    field->line = (struct http_slice){line, line_length + 2};
    field->name = (struct http_slice){line, colon};
    field->value = trim_header_value_whitespace(
        (struct http_slice){line + colon + 1, line_length - colon - 1});
    *offset += line_length + 2;
    return true;
}

/* Connection보다 앞서 나온 필드도 제거해야 하므로 전체 헤더를 확인한다.
 * 필드 이름은 대소문자 구별 없이 비교한다. 헤더 상한은 기존 8KiB다. */
static bool is_header_listed_in_connection(const unsigned char *header, size_t length,
                                 struct http_slice name) {
    size_t offset = find_header_line_end(header, length) + 2;
    struct header_field field;
    while (parse_next_header_field(header, length, &offset, &field)) {
        if (!header_name_matches(field.name, "connection")) continue;

        size_t position = 0;
        while (position < field.value.length) {
            size_t start = position;
            while (position < field.value.length && field.value.data[position] != ',') ++position;
            struct http_slice option = trim_header_value_whitespace(
                (struct http_slice){field.value.data + start, position - start});

            if (option.length == name.length &&
                strncasecmp((const char *)option.data, (const char *)name.data, name.length) == 0) {
                return true;
            }
            if (position < field.value.length) ++position;
        }
    }
    return false;
}

static bool is_connection_only_header(struct http_slice name) {
    return header_name_matches(name, "connection") || header_name_matches(name, "keep-alive") ||
           header_name_matches(name, "proxy-connection") || header_name_matches(name, "proxy-authenticate") ||
           header_name_matches(name, "proxy-authorization") || header_name_matches(name, "te") ||
           header_name_matches(name, "trailer") || header_name_matches(name, "transfer-encoding") ||
           header_name_matches(name, "upgrade");
}

static bool should_copy_header(const unsigned char *header, size_t length,
                                  struct http_slice name) {
    return !is_connection_only_header(name) && !header_name_matches(name, "content-length") &&
           !is_header_listed_in_connection(header, length, name);
}

static bool append_header_bytes(unsigned char *output, size_t *used,
                           const void *bytes, size_t length) {
    if (length > PROXY_WIRE_CAPACITY - *used) return false;
    memcpy(output + *used, bytes, length);
    *used += length;
    return true;
}

bool build_backend_request_headers(const unsigned char *header, size_t length,
                                  const struct http_request *request,
                                  unsigned char *output, size_t *output_length) {
    *output_length = 0;
    size_t request_line_length = find_header_line_end(header, length) + 2;
    if (!append_header_bytes(output, output_length, header, request_line_length)) return false;

    size_t offset = request_line_length;
    struct header_field field;
    while (parse_next_header_field(header, length, &offset, &field)) {
        /* Expect는 기존 서버 수신 단계에서 처리했다. 백엔드에 다시 넘기지 않는다. */
        if (!should_copy_header(header, length, field.name) || header_name_matches(field.name, "expect")) continue;
        if (!append_header_bytes(output, output_length, field.line.data, field.line.length)) return false;
    }

    /* Host를 제거하라는 요청은 원본 Host 유지 계약과 맞지 않아 거절한다. */
    const struct http_slice host = {(const unsigned char *)"Host", 4};
    if (is_header_listed_in_connection(header, length, host)) return false;

    char framing[96];
    int written = snprintf(framing, sizeof(framing),
                           "Content-Length: %zu\r\nConnection: close\r\n"
                           "Via: 1.1 jungle-proxy\r\n\r\n", request->body.length);
    return written > 0 && (size_t)written < sizeof(framing) &&
           append_header_bytes(output, output_length, framing, (size_t)written);
}

static bool parse_content_length_value(struct http_slice text, size_t *value) {
    *value = 0;
    if (text.length == 0) return false;

    for (size_t index = 0; index < text.length; ++index) {
        unsigned char byte = text.data[index];
        if (byte < '0' || byte > '9' || *value > (SIZE_MAX - (size_t)(byte - '0')) / 10) return false;
        *value = *value * 10 + (size_t)(byte - '0');
    }
    return true;
}

static bool is_valid_connection_header_value(struct http_slice value) {
    if (value.length == 0) return false;

    size_t position = 0;
    while (position < value.length) {
        while (position < value.length && (value.data[position] == ' ' || value.data[position] == '\t')) ++position;
        size_t start = position;
        while (position < value.length && is_http_token_character(value.data[position])) ++position;
        if (start == position) return false;

        while (position < value.length && (value.data[position] == ' ' || value.data[position] == '\t')) ++position;
        if (position == value.length) return true;
        if (value.data[position++] != ',' || position == value.length) return false;
    }
    return true;
}

static bool parse_backend_status_line(const unsigned char *header, size_t length,
                               struct proxy_response *response) {
    size_t line_length = find_header_line_end(header, length);
    if (line_length == SIZE_MAX || line_length < 13) return false;
    if (memcmp(header, "HTTP/1.1 ", 9) != 0 && memcmp(header, "HTTP/1.0 ", 9) != 0) return false;
    if (header[9] < '1' || header[9] > '5' || header[10] < '0' || header[10] > '9' ||
        header[11] < '0' || header[11] > '9' || header[12] != ' ') return false;

    for (size_t index = 13; index < line_length; ++index) {
        if ((header[index] < 32 && header[index] != '\t') || header[index] == 127) return false;
    }

    response->status_code = (header[9] - '0') * 100 + (header[10] - '0') * 10 + header[11] - '0';
    response->status_line_length = line_length + 2;
    return true;
}

static bool parse_backend_response_field(const struct header_field *field,
                                  struct proxy_response *response) {
    if (header_name_matches(field->name, "content-length")) {
        if (response->has_content_length ||
            !parse_content_length_value(field->value, &response->content_length)) return false;
        response->has_content_length = true;
    } else if (header_name_matches(field->name, "transfer-encoding")) {
        if (response->is_chunked || !header_name_matches(field->value, "chunked")) return false;
        response->is_chunked = true;
    } else if (header_name_matches(field->name, "connection")) {
        if (!is_valid_connection_header_value(field->value)) return false;
    }
    return true;
}

bool parse_backend_response_headers(const unsigned char *header, size_t length,
                                   struct proxy_response *response) {
    *response = (struct proxy_response){0};
    if (!parse_backend_status_line(header, length, response)) return false;

    size_t offset = response->status_line_length;
    struct header_field field;
    while (offset < length - 2) {
        if (!parse_next_header_field(header, length, &offset, &field)) return false;
        if (!parse_backend_response_field(&field, response)) return false;
    }

    if (offset != length - 2) return false;
    if (response->has_content_length && response->is_chunked) return false;
    if ((response->status_code < 200 || response->status_code == 204) &&
        (response->has_content_length || response->is_chunked)) return false;
    return response->status_code != 101;
}

static bool append_body_transfer_headers(unsigned char *output, size_t *output_length,
                                      const struct proxy_response *response) {
    bool no_body = response->status_code == 204 || response->status_code == 304;
    char framing[128];
    int written;
    if (no_body && !response->has_content_length) return true;

    if (response->has_content_length) {
        /* 304의 Content-Length는 실제 전송 길이가 아닌 표현 메타데이터다. */
        written = snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", response->content_length);
    } else {
        written = snprintf(framing, sizeof(framing), "Transfer-Encoding: chunked\r\n");
    }
    return written >= 0 && (size_t)written < sizeof(framing) &&
           append_header_bytes(output, output_length, framing, (size_t)written);
}

bool build_client_response_headers(const unsigned char *header, size_t length,
                                   const struct proxy_response *response,
                                   unsigned char *output, size_t *output_length) {
    *output_length = 0;
    if (!append_header_bytes(output, output_length, header, response->status_line_length)) return false;

    size_t offset = response->status_line_length;
    struct header_field field;
    while (parse_next_header_field(header, length, &offset, &field)) {
        if (!should_copy_header(header, length, field.name)) continue;
        if (!append_header_bytes(output, output_length, field.line.data, field.line.length)) return false;
    }

    if (!append_body_transfer_headers(output, output_length, response)) return false;
    static const char connection_headers[] = "Connection: close\r\nVia: 1.1 jungle-proxy\r\n\r\n";
    if (!append_header_bytes(output, output_length, connection_headers, sizeof(connection_headers) - 1)) return false;

    /* HTTP/1.0 응답도 client에는 HTTP/1.1로 표현해 EOF → chunked 전환을 지원. */
    if (*output_length >= 8) output[7] = '1';
    return true;
}

static int read_backend_response_headers(struct proxy_reader *reader,
                              unsigned char *header, size_t *length) {
    *length = 0;
    while (*length < PROXY_BUFFER_CAPACITY) {
        size_t line_length;
        if (read_backend_line(reader, header + *length,
                                    PROXY_BUFFER_CAPACITY - *length, &line_length) < 0) return -1;
        *length += line_length;
        if (line_length == 2) return 0;
    }
    errno = EPROTO;
    return -1;
}

int prepare_client_response_headers(struct proxy_reader *reader,
                                    struct proxy_response *response,
                                    unsigned char *output, size_t *output_length) {
    unsigned char header[PROXY_BUFFER_CAPACITY];
    size_t header_length;
    size_t interim_total = 0;

    for (;;) {
        if (read_backend_response_headers(reader, header, &header_length) < 0) return -1;
        if (!parse_backend_response_headers(header, header_length, response)) break;
        if (response->status_code >= 200) {
            if (build_client_response_headers(header, header_length, response, output, output_length)) return 0;
            break;
        }

        /* 최종 응답 전 1xx는 내부 소비하되 누적 헤더 한도를 유지한다. */
        interim_total += header_length;
        if (interim_total > PROXY_BUFFER_CAPACITY) break;
    }
    errno = EPROTO;
    return -1;
}

bool is_allowed_chunk_trailer_header(const unsigned char *line, size_t length) {
    size_t offset = 0;
    struct header_field field;
    return parse_next_header_field(line, length, &offset, &field) &&
           !is_connection_only_header(field.name) && !header_name_matches(field.name, "content-length") &&
           !header_name_matches(field.name, "host");
}
