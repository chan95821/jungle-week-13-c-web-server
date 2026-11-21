CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/server
TEST_NAMES = test_parse_port test_recv_header test_send_all test_http_parser
TEST_BINS = $(addprefix $(BUILD_DIR)/,$(TEST_NAMES))

SRCS = src/main.c src/server.c src/http.c src/router.c
OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
HEADERS = src/server.h src/http.h src/router.h
TEST_SRCS = src/server.c src/http.c src/router.c

.PHONY: all test verify clean server $(TEST_NAMES)

all: $(TARGET)

$(BUILD_DIR) $(OBJ_DIR):
	mkdir -p $@

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: src/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

server: $(TARGET)

$(TEST_NAMES): %: $(BUILD_DIR)/%

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

$(BUILD_DIR)/test_%: tests/unit/test_%.c $(TEST_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(ASAN_FLAGS) -o $@ $< $(TEST_SRCS)

verify: $(TARGET)
	python3 scripts/verify_iter1.py
	python3 scripts/verify_iter2.py

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BINS)
