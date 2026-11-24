CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/server
CGI_DIR = cgi-bin
CGI_BINS = $(CGI_DIR)/order
TEST_NAMES = test_parse_port test_recv_header test_send_all test_http_parser test_body_send test_proxy_selection test_proxy_handlers test_proxy_request test_proxy_validation test_proxy_flow test_proxy_acquire test_proxy_connect test_io_utils test_proxy_connect_timeout test_proxy_send test_proxy_receive test_http_response test_proxy_body test_proxy_config
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

HANDLER_SRCS = src/handlers/basic.c src/handlers/static.c src/handlers/greet.c src/handlers/cgi.c
PROXY_SRCS = src/handlers/proxy.c src/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_connect.c src/proxy_config.c
IO_SRCS = src/io_utils.c
SRCS = src/main.c src/server.c src/http.c src/router.c $(HANDLER_SRCS) $(PROXY_SRCS) $(IO_SRCS)
OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
HEADERS = src/server.h src/http.h src/router.h src/handlers/handlers.h src/proxy.h src/proxy_selection.h src/io_utils.h src/proxy_config.h
TEST_SRCS = src/server.c src/http.c src/router.c $(HANDLER_SRCS) $(IO_SRCS)

.PHONY: all test verify verify-edge clean server cgi $(TEST_NAMES)

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

$(BUILD_DIR)/test_body_send: tests/unit/test_body_send.c $(TEST_SRCS) $(PROXY_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< src/http.c src/router.c $(HANDLER_SRCS) $(PROXY_SRCS) $(IO_SRCS)

$(BUILD_DIR)/test_proxy_selection: tests/unit/test_proxy_selection.c src/proxy_selection.c src/proxy_selection.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< src/proxy_selection.c

$(BUILD_DIR)/test_proxy_acquire: tests/unit/test_proxy_acquire.c src/proxy.c $(TEST_SRCS) $(HEADERS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_config.c | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_config.c

$(BUILD_DIR)/test_proxy_connect_timeout: tests/unit/test_proxy_connect_timeout.c src/proxy_connect.c src/proxy_config.c $(HEADERS) $(IO_SRCS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(IO_SRCS)

$(BUILD_DIR)/test_proxy_send: tests/unit/test_proxy_send.c src/proxy.c $(TEST_SRCS) $(HEADERS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_connect.c src/proxy_config.c | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_config.c src/proxy_connect.c

$(BUILD_DIR)/test_proxy_receive: tests/unit/test_proxy_receive.c src/proxy.c $(TEST_SRCS) $(HEADERS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_connect.c src/proxy_config.c | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_config.c src/proxy_connect.c

$(BUILD_DIR)/test_proxy_body: tests/unit/test_proxy_body.c src/proxy.c $(TEST_SRCS) $(HEADERS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_connect.c src/proxy_config.c | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_config.c src/proxy_connect.c

$(BUILD_DIR)/test_proxy_handlers: tests/unit/test_proxy_handlers.c $(TEST_SRCS) $(HEADERS) src/handlers/proxy.c src/proxy.h src/proxy_selection.c src/proxy_selection.h src/proxy_validation.c src/proxy_config.c src/proxy_config.h | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS) src/handlers/proxy.c src/proxy_selection.c src/proxy_validation.c src/proxy_config.c

$(BUILD_DIR)/test_%: tests/unit/test_%.c $(TEST_SRCS) $(PROXY_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS) $(PROXY_SRCS)

verify: $(TARGET) $(CGI_BINS)
	python3 scripts/verify_iter1.py
	python3 scripts/verify_iter2.py
	python3 scripts/verify_iter3.py --root public --cafe pages/cafe.html --menu pages/menu.html
	python3 scripts/verify_iter3_dynamic.py
	python3 scripts/verify_iter4.py

verify-edge: $(TARGET) $(CGI_BINS)
	python3 scripts/verify_edge_cases.py

clean:
	rm -f $(OBJS) $(TARGET) $(CGI_BINS) $(TEST_BINS)
