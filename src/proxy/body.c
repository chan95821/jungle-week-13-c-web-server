#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

enum body_output { BODY_RAW_BYTES, BODY_AS_CHUNKS };

static int send_fixed_length_body_to_client(struct proxy_reader *reader, int client_fd,
                              size_t remaining, enum body_output output);
static int send_chunked_body_to_client(struct proxy_reader *reader, int client_fd);
static int send_body_until_backend_closes(struct proxy_reader *reader, int client_fd);

/* 최종 응답 헤더를 이미 보낸 뒤 호출한다. 어느 오류 경로에서도 새 HTTP
 * 오류 응답을 만들지 않는다. 실패는 호출자에게 -1로만 전달한다. */
int send_backend_response_body_to_client(struct proxy_reader *reader, int client_fd,
                               const struct proxy_response *response) {
    if (response->status_code == 204 || response->status_code == 304) return 0;
    if (response->has_content_length) {
        return send_fixed_length_body_to_client(reader, client_fd, response->content_length, BODY_RAW_BYTES);
    }
    if (response->is_chunked) return send_chunked_body_to_client(reader, client_fd);
    return send_body_until_backend_closes(reader, client_fd);
}

static int send_body_chunk_to_client(int client_fd, const unsigned char *bytes, size_t length,
                        const volatile sig_atomic_t *running) {
    char size_line[32];
    int written = snprintf(size_line, sizeof(size_line), "%zx\r\n", length);
    if (written < 0 || (size_t)written >= sizeof(size_line)) {
        errno = EOVERFLOW;
        return -1;
    }

    if (send_all_bytes(client_fd, size_line, (size_t)written, PROXY_CLIENT_TIMEOUT_MS, running) < 0) return -1;
    if (send_all_bytes(client_fd, bytes, length, PROXY_CLIENT_TIMEOUT_MS, running) < 0) return -1;
    return send_all_bytes(client_fd, "\r\n", 2, PROXY_CLIENT_TIMEOUT_MS, running);
}

static int send_chunked_body_end_to_client(int client_fd, const volatile sig_atomic_t *running) {
    return send_all_bytes(client_fd, "0\r\n\r\n", 5, PROXY_CLIENT_TIMEOUT_MS, running);
}

/* 헤더와 함께 도착해 reader에 남은 본문부터 소비한다. count는 바이트
 * 길이이며 NUL을 포함해도 전송이 끊기지 않는다. 현재 조각을 다 보낸 뒤
 * 다음 recv를 하므로 reader 버퍼를 송신 도중 덮어쓰지 않는다. */
static int send_fixed_length_body_to_client(struct proxy_reader *reader, int client_fd,
                              size_t remaining, enum body_output output) {
    while (remaining > 0) {
        if (reader->position == reader->length) {
            ssize_t received = receive_backend_bytes(reader);
            if (received < 0) return -1;
            if (received == 0) {
                errno = EPROTO;
                return -1;
            }
        }

        size_t count = reader->length - reader->position;
        if (count > remaining) count = remaining;
        const unsigned char *bytes = reader->buffer + reader->position;

        int result;
        if (output == BODY_AS_CHUNKS) {
            result = send_body_chunk_to_client(client_fd, bytes, count, reader->running);
        } else {
            result = send_all_bytes(client_fd, bytes, count,
                                       PROXY_CLIENT_TIMEOUT_MS, reader->running);
        }
        if (result < 0) return -1;

        reader->position += count;
        remaining -= count;
    }
    return 0;
}

static int parse_hex_digit(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static void skip_chunk_extension_whitespace(const unsigned char *line, size_t length,
                                      size_t *position) {
    while (*position < length && (line[*position] == ' ' || line[*position] == '\t')) ++*position;
}

static bool parse_quoted_chunk_extension_value(const unsigned char *line, size_t length,
                                  size_t *position) {
    ++*position; /* 여는 따옴표 */
    while (*position < length) {
        unsigned char byte = line[(*position)++];
        if (byte == '"') return true;
        if (byte == '\\') {
            if (*position == length) return false;
            byte = line[(*position)++];
        }
        if ((byte < 32 && byte != '\t') || byte == 127) return false;
    }
    return false;
}

static bool parse_chunk_extension_value(const unsigned char *line, size_t length,
                                     size_t *position) {
    skip_chunk_extension_whitespace(line, length, position);
    if (*position < length && line[*position] == '"') {
        return parse_quoted_chunk_extension_value(line, length, position);
    }

    size_t start = *position;
    while (*position < length && is_http_token_character(line[*position])) ++*position;
    return *position != start;
}

/* extension은 이름과 선택적 값의 문법만 확인하고 전달하지 않는다.
 * 숫자 크기 파싱과 분리해 세미콜론·이름·값의 순서를 그대로 읽을 수 있다. */
static bool validate_chunk_extensions(const unsigned char *line, size_t length,
                                    size_t position) {
    while (position < length) {
        skip_chunk_extension_whitespace(line, length, &position);
        if (position == length || line[position++] != ';') return false;

        skip_chunk_extension_whitespace(line, length, &position);
        size_t name_start = position;
        while (position < length && is_http_token_character(line[position])) ++position;
        if (position == name_start) return false;

        skip_chunk_extension_whitespace(line, length, &position);
        if (position < length && line[position] == '=') {
            ++position;
            if (!parse_chunk_extension_value(line, length, &position)) return false;
        }
    }
    return true;
}

bool parse_chunk_size_line(const unsigned char *line, size_t length, size_t *size) {
    *size = 0;
    size_t position = 0;
    while (position < length) {
        int digit = parse_hex_digit(line[position]);
        if (digit < 0) break;
        if (*size > (SIZE_MAX - (unsigned int)digit) / 16) return false;
        *size = *size * 16 + (unsigned int)digit;
        ++position;
    }
    return position > 0 && validate_chunk_extensions(line, length, position);
}

static int read_chunk_data_crlf(struct proxy_reader *reader) {
    unsigned char carriage_return, line_feed;
    if (read_backend_byte(reader, &carriage_return) < 0 ||
        read_backend_byte(reader, &line_feed) < 0) return -1;
    if (carriage_return != '\r' || line_feed != '\n') {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int read_chunk_trailer_headers(struct proxy_reader *reader, unsigned char *line) {
    size_t total_length = 0;
    for (;;) {
        size_t line_length;
        if (read_backend_line(reader, line,
                                    PROXY_BUFFER_CAPACITY - total_length, &line_length) < 0) return -1;
        total_length += line_length;
        if (line_length == 2) return 0;
        if (!is_allowed_chunk_trailer_header(line, line_length)) {
            errno = EPROTO;
            return -1;
        }
    }
}

/* 청크를 해제하고 버퍼 단위로 재청크한다. 모든 청크·트레일러를 정상
 * 소비한 경우에만 완료 표시를 보낸다. 오류면 최종 0청크를 보내지 않는다. */
static int send_chunked_body_to_client(struct proxy_reader *reader, int client_fd) {
    unsigned char line[PROXY_BUFFER_CAPACITY];
    for (;;) {
        size_t line_length, chunk_length;
        if (read_backend_line(reader, line, sizeof(line), &line_length) < 0) return -1;
        if (!parse_chunk_size_line(line, line_length - 2, &chunk_length)) {
            errno = EPROTO;
            return -1;
        }
        if (chunk_length == 0) break;

        if (send_fixed_length_body_to_client(reader, client_fd, chunk_length, BODY_AS_CHUNKS) < 0) return -1;
        if (read_chunk_data_crlf(reader) < 0) return -1;
    }

    if (read_chunk_trailer_headers(reader, line) < 0) return -1;
    return send_chunked_body_end_to_client(client_fd, reader->running);
}

static int send_body_until_backend_closes(struct proxy_reader *reader, int client_fd) {
    for (;;) {
        if (reader->position != reader->length) {
            if (send_body_chunk_to_client(client_fd, reader->buffer + reader->position,
                             reader->length - reader->position, reader->running) < 0) return -1;
            reader->position = reader->length;
        }

        ssize_t received = receive_backend_bytes(reader);
        if (received < 0) return -1;
        if (received == 0) return send_chunked_body_end_to_client(client_fd, reader->running);
    }
}
