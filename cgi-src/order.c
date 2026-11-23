#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Decode form-urlencoded slice into NUL-terminated dst; reject bad '%' syntax, NUL, or overflow. */
static bool decode_component(const char *src, size_t len, char *dst, size_t cap) {
    size_t out = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= len) return false;
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi < 0 || lo < 0) return false;
            int byte = (hi << 4) | lo;
            if (byte == 0) return false;
            c = (char)byte;
            i += 2;
        }
        if (out + 1 >= cap) return false;
        dst[out++] = c;
    }

    dst[out] = '\0';
    return true;
}

/* Extract first "item" and "qty" from QUERY_STRING while validating all query segments. */
static bool parse_order_query(const char *query, char *item, size_t item_cap,
                              char *qty_buf, size_t qty_cap) {
    item[0] = '\0';
    qty_buf[0] = '\0';

    bool got_item = false, got_qty = false;
    size_t len = query ? strlen(query) : 0;
    size_t pos = 0;

    while (pos < len) {
        size_t end = pos;
        while (end < len && query[end] != '&') ++end;

        if (end > pos) {
            size_t eq = pos;
            while (eq < end && query[eq] != '=') ++eq;

            char key[64];
            if (!decode_component(query + pos, eq - pos, key, sizeof(key))) return false;

            const char *val_src = eq < end ? query + eq + 1 : "";
            size_t val_len = eq < end ? end - eq - 1 : 0;
            if (!got_item && strcmp(key, "item") == 0) {
                if (!decode_component(val_src, val_len, item, item_cap)) return false;
                got_item = true;
            } else if (!got_qty && strcmp(key, "qty") == 0) {
                if (!decode_component(val_src, val_len, qty_buf, qty_cap)) return false;
                got_qty = true;
            } else {
                char scratch[256];
                if (!decode_component(val_src, val_len, scratch, sizeof(scratch))) return false;
            }
        }

        pos = end + 1;
    }

    return true;
}

/* Parse decimal quantity in 1..99. Return false on non-digits or out-of-range values. */
static bool parse_qty(const char *s, int *out_qty) {
    if (!s || s[0] == '\0') return false;
    int value = 0;
    for (size_t i = 0; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (s[i] - '0');
        if (value > 99) return false;
    }
    if (value < 1) return false;
    *out_qty = value;
    return true;
}

static void print_page_head(const char *title) {
    printf("<!doctype html>\n"
           "<html lang=\"ko\">\n"
           "<head>\n"
           "  <meta charset=\"utf-8\">\n"
           "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "  <title>%s | 정글 카페</title>\n"
           "  <link rel=\"stylesheet\" href=\"/assets/cafe.css\">\n"
           "</head>\n"
           "<body>\n"
           "  <main>\n"
           "    <p class=\"eyebrow\">JUNGLE CAFE · CGI</p>\n",
           title);
}

static void print_page_tail(void) {
    printf("    <p class=\"nav-links\"><a href=\"/menu\">← 메뉴로</a> · "
           "<a href=\"/cafe\">카페 소개</a></p>\n"
           "  </main>\n"
           "</body>\n"
           "</html>\n");
}

/* Read QUERY_STRING, validate item/qty, and emit CGI headers plus HTML order estimate. */
int main(void) {
    const char *query = getenv("QUERY_STRING");
    char item[64];
    char qty_buf[32];
    if (!parse_order_query(query, item, sizeof(item), qty_buf, sizeof(qty_buf))) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/html; charset=utf-8\r\n\r\n");
        print_page_head("주문 오류");
        printf("    <section id=\"order-result\" class=\"result-card error\">\n"
               "      <h2>잘못된 요청입니다</h2>\n"
               "      <p>품목과 수량 형식을 다시 확인해 주세요.</p>\n"
               "    </section>\n");
        print_page_tail();
        return 0;
    }

    const char *label = NULL;
    int unit_price = 0;
    int prep_minutes = 0;
    if (strcmp(item, "americano") == 0) {
        label = "아메리카노"; unit_price = 4000; prep_minutes = 3;
    } else if (strcmp(item, "latte") == 0) {
        label = "카페 라테"; unit_price = 4500; prep_minutes = 4;
    } else if (strcmp(item, "tea") == 0) {
        label = "허브 티"; unit_price = 4000; prep_minutes = 3;
    }

    int qty = 0;
    if (!label || !parse_qty(qty_buf, &qty)) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/html; charset=utf-8\r\n\r\n");
        print_page_head("주문 확인 실패");
        printf("    <section id=\"order-result\" class=\"result-card error\">\n"
               "      <h2>주문 항목을 확인할 수 없습니다</h2>\n"
               "      <p>메뉴(americano, latte, tea)와 수량(1~99)을 올바르게 선택해 주세요.</p>\n"
               "    </section>\n");
        print_page_tail();
        return 0;
    }

    int total = unit_price * qty;
    int eta = prep_minutes + (qty - 1);

    printf("Content-Type: text/html; charset=utf-8\r\n\r\n");
    print_page_head("주문 견적");
    fflush(stdout);

    printf("    <section id=\"order-result\" class=\"result-card\">\n"
           "      <h2>주문 견적 확인</h2>\n"
           "      <p><strong>%s</strong> %d잔 (잔당 %d원)</p>\n"
           "      <p>총 금액: <strong>%d원</strong> · 예상 준비 시간: 약 %d분</p>\n"
           "    </section>\n",
           label, qty, unit_price, total, eta);
    print_page_tail();

    return 0;
}
