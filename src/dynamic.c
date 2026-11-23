#include "dynamic.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 정적 파일의 경로 디코더와 목적이 다르다. 여기서는 form 규칙에 따라 +를
 * 공백으로 바꾼다. 서로 다른 입력 규칙을 가진 코드를 억지로 공유하지 않는다. */
static int form_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int decode_form_component(struct http_slice input, char *output, size_t capacity,
                                  size_t *length) {
    size_t used = 0;
    for (size_t i = 0; i < input.length; ++i) {
        unsigned char c = input.data[i];
        if (c == '%') {
            if (input.length - i < 3) return 400;
            int high = form_hex(input.data[i + 1]);
            int low = form_hex(input.data[i + 2]);
            if (high < 0 || low < 0) return 400;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        /* 이 API의 키와 정수 값에는 제어 문자/NUL이 필요하지 않다.
         * 출력이 문자열이 될 때의 종료 문자 한 칸도 항상 남긴다. */
        if (c < 32 || c == 127 || used + 1 >= capacity) return 400;
        output[used++] = (char)c;
    }
    output[used] = '\0';
    *length = used;
    return 0;
}

static int parse_integer(const char *text, size_t length, int64_t *out) {
    if (length == 0) return 400;
    size_t i = 0;
    int negative = text[0] == '-';
    if (negative || text[0] == '+') ++i;
    if (i == length) return 400;

    /* INT64_MIN의 절댓값은 INT64_MAX보다 1 크다. 양의 signed 정수로 먼저
     * 만들면 오버플로우하므로 unsigned 크기로 누적한 뒤 부호를 적용한다. */
    uint64_t limit = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
    uint64_t magnitude = 0;
    for (; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return 400;
        uint64_t digit = (uint64_t)(text[i] - '0');
        if (magnitude > (limit - digit) / 10) return 400;
        magnitude = magnitude * 10 + digit;
    }
    if (negative) {
        *out = magnitude == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)magnitude;
    } else {
        *out = (int64_t)magnitude;
    }
    return 0;
}

static int parse_add_parameters(struct http_slice body, int64_t *a, int64_t *b) {
    if (!body.data || body.length == 0) return 400;
    unsigned int seen = 0;
    size_t offset = 0;
    while (offset < body.length) {
        size_t end = offset;
        while (end < body.length && body.data[end] != '&') ++end;
        size_t equal = offset;
        while (equal < end && body.data[equal] != '=') ++equal;
        if (equal == offset || equal == end) return 400;

        /* 원본 body는 NUL 종료가 없으므로 명시적인 길이로 구간을 나눈다.
         * a/b 키는 한 글자다. 정수 표기는 종료 문자를 제외하고 최대 31바이트를 받는다.
         * 64비트 정수의 일반적인 표기는 부호를 포함해 최대 20글자다. */
        char key[2], value[32];
        size_t key_length, value_length;
        int status = decode_form_component(
            (struct http_slice){body.data + offset, equal - offset}, key, sizeof(key), &key_length);
        if (status) return status;
        status = decode_form_component(
            (struct http_slice){body.data + equal + 1, end - equal - 1}, value, sizeof(value), &value_length);
        if (status) return status;
        if (key_length != 1 || (key[0] != 'a' && key[0] != 'b')) return 400;
        unsigned int bit = key[0] == 'a' ? 1u : 2u;
        if (seen & bit) return 400;
        status = parse_integer(value, value_length, key[0] == 'a' ? a : b);
        if (status) return status;
        seen |= bit;
        if (end == body.length) break;
        offset = end + 1;
        if (offset == body.length) return 400;
    }
    return seen == 3u ? 0 : 400;
}

int dynamic_check_headers(const struct http_request *request, enum http_route route) {
    if (route != HTTP_ROUTE_ADD) return 0;
    if (request->duplicate_content_type) return 400;
    struct http_slice type = request->content_type;
    size_t length = 0;
    while (length < type.length && type.data[length] != ';') ++length;
    while (length && (type.data[length - 1] == ' ' || type.data[length - 1] == '\t')) --length;
    static const char expected[] = "application/x-www-form-urlencoded";
    if (length != sizeof(expected) - 1) return 415;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = type.data[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)expected[i]) return 415;
    }
    return 0;
}

int prepare_dynamic_response(const struct http_request *request, enum http_route route,
                              char *buffer, size_t capacity, struct router_response *response) {
    int status = dynamic_check_headers(request, route);
    if (status) return status;
    int length;
    if (route == HTTP_ROUTE_TIME) {
        /* time은 표준 C 라이브러리 함수다. 기존 CLOCK_MONOTONIC은 대기 기한용이고,
         * 여기서는 실제 현재 시각을 얻는다. 지역 시각 문자열 대신 Unix 초를 보낸다. */
        time_t now = time(NULL);
        if (now == (time_t)-1) return 500;
        length = snprintf(buffer, capacity, "{\"timestamp\":%" PRIdMAX "}\n", (intmax_t)now);
    } else if (route == HTTP_ROUTE_ADD) {
        int64_t a = 0, b = 0;
        status = parse_add_parameters(request->body, &a, &b);
        if (status) return status;
        /* signed 덧셈 후에 검사하면 이미 정의되지 않은 동작이 될 수 있으므로,
         * 실제 a+b를 계산하기 전에 결과가 정수 범위 안인지 확인한다. */
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return 400;
        length = snprintf(buffer, capacity, "{\"result\":%" PRId64 "}\n", a + b);
    } else {
        return 500;
    }
    /* snprintf는 잘린 경우에도 원래 필요했던 길이를 반환한다.
     * 버퍼보다 긴 결과를 그대로 send하지 않도록 반환값의 상한도 검사한다. */
    if (length < 0 || (size_t)length >= capacity) return 500;
    *response = (struct router_response){
        .status = 200,
        .content_type = "application/json",
        .body = buffer,
        .length = (size_t)length,
        .allow = NULL
    };
    return 0;
}
