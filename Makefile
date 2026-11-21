CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2
ASAN_FLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined

TARGET = server
TEST_BINS = test_parse_port test_recv_header test_send_all test_http test_recv_body

SRCS = src/main.c src/server.c src/http.c
OBJS = $(SRCS:.c=.o)

.PHONY: all test verify verify-iter1 sanitize clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c src/server.h src/http.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do ./$$t || exit 1; done

test_%: tests/unit/test_%.c src/server.c src/server.h src/http.c src/http.h
	$(CC) $(ASAN_FLAGS) -o $@ $< src/server.c src/http.c

verify: $(TARGET)
	python3 scripts/verify_iter2.py

verify-iter1: $(TARGET)
	python3 scripts/verify_iter1.py

# 실제 서버의 오류 경로도 sanitizer로 검사한다. clean 뒤에 순서대로 빌드한다.
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(ASAN_FLAGS)' all test verify

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BINS)
