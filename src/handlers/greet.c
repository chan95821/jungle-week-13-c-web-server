#include "handlers.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Convert ASCII hex digit to 0..15, or -1 if invalid. */
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

/* Split query on '&' and first '=', validate %XX/%00 across all params, and extract first "name". */
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

/* Return true if slice contains any ASCII control character (0x01..0x1F or 0x7F). */
static bool has_control_char(const unsigned char *data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = data[i];
        if ((c >= 0x01 && c <= 0x1F) || c == 0x7F) return true;
    }
    return false;
}

/* Trim leading and trailing ASCII space (' ') bytes without modifying buffer. */
static struct http_slice trim_ascii_spaces(const unsigned char *data, size_t length) {
    size_t start = 0;
    while (start < length && data[start] == ' ') ++start;
    size_t end = length;
    while (end > start && data[end - 1] == ' ') --end;
    return (struct http_slice){data + start, end - start};
}

/* Map the 5 HTML-sensitive characters (&, <, >, ", ') to entity literals. */
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

/* Compute HTML-escaped byte length with size_t overflow check. */
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

/* Copy text into dst while escaping HTML-sensitive characters and preserving UTF-8/raw bytes. */
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

/* Allocate and assemble dynamic HTML greeting page. Return false on overflow or OOM. */
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

/* Extract and validate query "name" (fallback "손님"), then prepare owned HTML response. */
void prepare_greet(const struct http_request *request,
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
    if (!router_prepare_owned_memory(response, 200, "text/html; charset=utf-8",
                                     html, html_len)) {
        router_prepare_error(response, 500, NULL);
    }
}
