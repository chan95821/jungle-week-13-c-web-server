CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
THREAD_FLAGS = -pthread

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/server
BACKEND_TARGET = $(BUILD_DIR)/test_backend
CGI_DIR = cgi-bin
CGI_BINS = $(CGI_DIR)/order
TEST_NAMES = test_parse_port test_recv_header test_send_all test_http_parser test_body_send test_proxy_selection test_proxy_protocol test_proxy_lifecycle
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

HANDLER_SRCS = src/handlers/basic.c src/handlers/static.c src/handlers/greet.c src/handlers/cgi.c
PROXY_SRCS = src/proxy.c src/proxy_transfer.c src/proxy_io.c src/proxy_headers.c src/proxy_body.c
EVENT_SRCS = src/event_server.c src/event_worker.c src/event_connection.c src/event_proxy.c src/local_work.c
SRCS = src/main.c src/server.c src/socket_io.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS) $(EVENT_SRCS)
LIFECYCLE_TEST_OBJS = $(patsubst src/%.c,$(BUILD_DIR)/test-obj/%.o,$(PROXY_SRCS))
OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
HEADERS = src/server.h src/socket_io.h src/http.h src/router.h src/proxy.h src/proxy_internal.h src/handlers/handlers.h src/event_server.h src/event_internal.h
TEST_SRCS = src/server.c src/socket_io.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)

.PHONY: all test verify verify-edge clean server cgi backend $(TEST_NAMES)

all: $(TARGET) $(CGI_BINS)

$(BUILD_DIR) $(OBJ_DIR) $(CGI_DIR):
	mkdir -p $@

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $@ $(OBJS)

$(CGI_DIR)/%: cgi-src/%.c | $(CGI_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OBJ_DIR)/%.o: src/%.c $(HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -c -o $@ $<

server: $(TARGET)

cgi: $(CGI_BINS)

$(TEST_NAMES): %: $(BUILD_DIR)/%

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

$(BUILD_DIR)/test_body_send: tests/unit/test_body_send.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< src/socket_io.c src/http.c src/router.c $(PROXY_SRCS) $(HANDLER_SRCS)

$(BUILD_DIR)/test_%: tests/unit/test_%.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS)

verify: $(TARGET) $(CGI_BINS) $(BACKEND_TARGET)
	python3 scripts/verify_iter1.py
	python3 scripts/verify_iter2.py
	python3 scripts/verify_iter3.py --root public --cafe pages/cafe.html --menu pages/menu.html
	python3 scripts/verify_iter3_dynamic.py
	python3 scripts/verify_iter4.py
	python3 scripts/verify_edge_cases.py

clean:
	rm -f $(OBJS) $(LIFECYCLE_TEST_OBJS) $(TARGET) $(CGI_BINS) $(TEST_BINS) $(BACKEND_TARGET)

backend: $(BACKEND_TARGET)

$(BACKEND_TARGET): tests/backends/server.c src/socket_io.c src/socket_io.h src/http.c src/http.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< src/socket_io.c src/http.c

$(BUILD_DIR)/test_proxy_protocol: tests/unit/test_proxy_protocol.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS)

verify-edge: $(TARGET) $(BACKEND_TARGET)
	python3 scripts/verify_edge_cases.py

# Allocation injection uses separate objects; no GNU linker-only --wrap dependency.
$(BUILD_DIR)/test-obj/%.o: src/%.c $(HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(ASAN_FLAGS) -Dmalloc=proxy_test_malloc -Dcalloc=proxy_test_calloc -Dfree=proxy_test_free -c -o $@ $<

$(BUILD_DIR)/test_proxy_lifecycle: tests/unit/test_proxy_lifecycle.c $(TEST_SRCS) $(HEADERS) $(LIFECYCLE_TEST_OBJS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(filter-out $(PROXY_SRCS),$(TEST_SRCS)) $(LIFECYCLE_TEST_OBJS)
