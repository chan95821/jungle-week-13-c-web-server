CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined

TARGET = server
TEST_BINS = test_parse_port test_recv_header test_send_all

SRCS = src/main.c src/server.c
OBJS = $(SRCS:.c=.o)

.PHONY: all test verify clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c src/server.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

test_%: tests/unit/test_%.c src/server.c src/server.h
	$(CC) $(ASAN_FLAGS) -o $@ $< src/server.c

verify: $(TARGET)
	python3 scripts/verify_iter1.py

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BINS)
