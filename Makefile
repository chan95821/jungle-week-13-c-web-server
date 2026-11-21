CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined

BUILD_DIR = build
TARGET = server
TEST_NAMES = test_parse_port test_recv_header test_send_all test_parse_request test_recv_body test_dispatch
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

LIB_SRCS = src/server.c src/http_parse.c src/router.c
SRCS = src/main.c $(LIB_SRCS)
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all test verify clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: src/%.c src/server.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

$(BUILD_DIR)/test_%: tests/unit/test_%.c $(LIB_SRCS) src/server.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(LIB_SRCS)

verify: $(TARGET)
	python3 scripts/verify_iter2.py

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
