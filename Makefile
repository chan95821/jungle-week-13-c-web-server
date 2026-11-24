CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/server
CGI_DIR = cgi-bin
CGI_BINS = $(CGI_DIR)/order
TEST_NAMES = test_parse_port test_recv_header test_send_all test_http_parser test_body_send test_proxy
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

HANDLER_SRCS = src/handlers/basic.c src/handlers/static.c src/handlers/greet.c src/handlers/cgi.c
PROXY_SRCS = src/proxy.c src/proxy/io.c src/proxy/headers.c src/proxy/body.c
PROXY_HEADERS = src/proxy.h src/proxy/internal.h
SRCS = src/main.c src/server.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)
OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
HEADERS = src/server.h src/http.h src/router.h $(PROXY_HEADERS) src/handlers/handlers.h
TEST_SRCS = src/server.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)

.PHONY: all test verify verify-proxy clean server cgi $(TEST_NAMES)

all: $(TARGET) $(CGI_BINS)

$(BUILD_DIR) $(OBJ_DIR) $(CGI_DIR):
	mkdir -p $@

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(CGI_DIR)/%: cgi-src/%.c | $(CGI_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OBJ_DIR)/%.o: src/%.c $(HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

server: $(TARGET)

cgi: $(CGI_BINS)

$(TEST_NAMES): %: $(BUILD_DIR)/%

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

$(BUILD_DIR)/test_body_send: tests/unit/test_body_send.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)

$(BUILD_DIR)/test_proxy: tests/unit/test_proxy.c $(PROXY_SRCS) $(PROXY_HEADERS) src/http.c src/http.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(PROXY_SRCS) src/http.c

$(BUILD_DIR)/test_%: tests/unit/test_%.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS)

verify: $(TARGET) $(CGI_BINS)
	python3 scripts/verify_iter1.py
	python3 scripts/verify_iter2.py
	python3 scripts/verify_iter3.py --root public --cafe pages/cafe.html --menu pages/menu.html
	python3 scripts/verify_iter3_dynamic.py

# 기존 회귀와 별도인 기능 검증 대상. 실행 범위 확정 후에만 호출한다.
verify-proxy: $(TARGET)
	python3 scripts/verify_iter4.py

clean:
	rm -f $(OBJS) $(TARGET) $(CGI_BINS) $(TEST_BINS)
