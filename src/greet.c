#include "greet.h"

#include <stdlib.h>
#include <string.h>

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Form-style query decoding only: static file paths use a different decoder. */
static int next_byte(struct http_slice raw, size_t *offset, unsigned char *out) {
    unsigned char c = raw.data[(*offset)++];
    if (c == '%') {
        if (raw.length - *offset < 2) return 400;
        int high = hex_value(raw.data[*offset]);
        int low = hex_value(raw.data[*offset + 1]);
        if (high < 0 || low < 0) return 400;
        *offset += 2;
        c = (unsigned char)(high * 16 + low);
    } else if (c == '+') {
        c = ' ';
    }
    *out = c;
    return 0;
}

static bool forbidden_codepoint(uint32_t codepoint) {
    /* Current checkpoint policy; widening requires a user decision. */
    return codepoint <= 0x1f || (codepoint >= 0x7f && codepoint <= 0x9f);
}

/* Validate without allocating or changing input; also match a decoded key. */
static int validate_component(struct http_slice raw, size_t *decoded_length,
                              bool *is_name) {
    size_t offset = 0, length = 0;
    unsigned int continuation = 0;
    uint32_t codepoint = 0, minimum = 0;
    bool matches = true;
    while (offset < raw.length) {
        unsigned char c;
        if (next_byte(raw, &offset, &c)) return 400;
        if (length >= 4 || c != (unsigned char)"name"[length]) matches = false;
        ++length;
        if (!continuation) {
            if (c < 0x80) {
                if (forbidden_codepoint(c)) return 400;
            } else if (c >= 0xc2 && c <= 0xdf) {
                codepoint = c & 0x1f;
                minimum = 0x80;
                continuation = 1;
            } else if (c >= 0xe0 && c <= 0xef) {
                codepoint = c & 0x0f;
                minimum = 0x800;
                continuation = 2;
            } else if (c >= 0xf0 && c <= 0xf4) {
                codepoint = c & 0x07;
                minimum = 0x10000;
                continuation = 3;
            } else return 400;
        } else {
            if ((c & 0xc0) != 0x80) return 400;
            codepoint = (codepoint << 6) | (c & 0x3f);
            --continuation;
            if (!continuation &&
                (codepoint < minimum || codepoint > 0x10ffff ||
                 (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
                 forbidden_codepoint(codepoint))) return 400;
        }
    }
    if (continuation) return 400;
    *decoded_length = length;
    *is_name = matches && length == 4;
    return 0;
}

struct router_response greet_prepare_response(const struct http_request *request,
                                               unsigned char **owned_body) {
    *owned_body = NULL;
    struct http_slice name = {(const unsigned char *)"World", 5};
    size_t name_length = 5;
    bool name_seen = false;
    size_t offset = request->path.length;
    if (offset < request->target.length) ++offset; /* Original '?' only. */
    while (offset < request->target.length) {
        size_t end = offset;
        while (end < request->target.length && request->target.data[end] != '&') ++end;
        if (end != offset) {
            size_t equals = offset;
            while (equals < end && request->target.data[equals] != '=') ++equals;
            struct http_slice key = {request->target.data + offset, equals - offset};
            size_t value_start = equals < end ? equals + 1 : end;
            struct http_slice value = {request->target.data + value_start, end - value_start};
            size_t key_length, value_length;
            bool is_name, unused;
            if (validate_component(key, &key_length, &is_name) ||
                validate_component(value, &value_length, &unused))
                return router_prepare_response(request, HTTP_ROUTE_GREET, 400, NULL);
            if (is_name) {
                if (name_seen || !value_length)
                    return router_prepare_response(request, HTTP_ROUTE_GREET, 400, NULL);
                name_seen = true;
                name = value;
                name_length = value_length;
            }
        }
        offset = end + 1;
    }
    if (name_length > SIZE_MAX - 9)
        return router_prepare_response(request, HTTP_ROUTE_GREET, 500, NULL);
    size_t length = name_length + 9; /* "Hello, " + name + "!\n" */
    unsigned char *body = malloc(length);
    if (!body) return router_prepare_response(request, HTTP_ROUTE_GREET, 500, NULL);
    memcpy(body, "Hello, ", 7);
    size_t input = 0, output = 7;
    while (input < name.length) {
        unsigned char c;
        if (next_byte(name, &input, &c)) {
            free(body);
            return router_prepare_response(request, HTTP_ROUTE_GREET, 400, NULL);
        }
        body[output++] = c;
    }
    memcpy(body + output, "!\n", 2);
    *owned_body = body;
    struct router_response response =
        router_prepare_response(request, HTTP_ROUTE_GREET, 200, NULL);
    response.content_type = "text/plain; charset=utf-8";
    response.body = body;
    response.length = length;
    return response;
}