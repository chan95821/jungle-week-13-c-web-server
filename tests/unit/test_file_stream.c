#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static ssize_t scripted_read(int fd, void *buffer, size_t length);
static ssize_t scripted_send(int fd, const void *buffer, size_t length, int flags);

/* Keep test-only I/O substitution out of production; inspect private sender. */
#define read scripted_read
#define send scripted_send
#include "../../src/server.c"
#undef read
#undef send

static unsigned char source[FILE_SEND_BUFFER_BYTES + 1], captured[32768];
static size_t available, position, captured_length, read_failure, send_failure;
static bool interrupt_read, interrupt_send, real_file_read;

static void reset(size_t count) {
    available = count;
    position = captured_length = 0;
    read_failure = send_failure = SIZE_MAX;
    interrupt_read = interrupt_send = real_file_read = false;
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)i;
}

static ssize_t scripted_read(int fd, void *buffer, size_t length) {
    if (real_file_read) return read(fd, buffer, length);
    (void)fd;
    if (interrupt_read) { interrupt_read = false; errno = EINTR; return -1; }
    if (position >= read_failure) { errno = EIO; return -1; }
    if (length > available - position) length = available - position;
    if (length > 1021) length = 1021; /* Deterministic short read. */
    memcpy(buffer, source + position, length);
    position += length;
    return (ssize_t)length;
}

static ssize_t scripted_send(int fd, const void *buffer, size_t length, int flags) {
    (void)fd;
    (void)flags;
    if (interrupt_send) { interrupt_send = false; errno = EINTR; return -1; }
    if (captured_length >= send_failure) { errno = EPIPE; return -1; }
    if (length > send_failure - captured_length) length = send_failure - captured_length;
    if (length > 97) length = 97; /* Deterministic short send. */
    assert(captured_length + length <= sizeof(captured));
    memcpy(captured + captured_length, buffer, length);
    captured_length += length;
    return (ssize_t)length;
}

int main(void) {
    reset(sizeof(source));
    interrupt_read = interrupt_send = true;
    assert(send_file_body(1, 2, sizeof(source), NULL) == 0);
    assert(captured_length == sizeof(source) && memcmp(captured, source, sizeof(source)) == 0);
    reset(9); /* Growth beyond the declared size is excluded. */
    assert(send_file_body(1, 2, 5, NULL) == 0 && position == 5 && captured_length == 5);
    reset(3); /* Truncated after initial size. */
    assert(send_file_body(1, 2, 5, NULL) == -1 && errno == EIO && captured_length == 3);
    reset(5);
    read_failure = 0;
    assert(send_file_body(1, 2, 5, NULL) == -1 && captured_length == 0);
    reset(5);
    send_failure = 2;
    assert(send_file_body(1, 2, 5, NULL) == -1 && errno == EPIPE && captured_length == 2);
    reset(0);
    assert(send_file_body(1, 2, 0, NULL) == 0 && captured_length == 0);
    reset(3);
    struct router_response response = {
        .status = 200, .content_type = "application/octet-stream",
        .length = 5, .kind = ROUTER_BODY_FILE, .file_fd = 2
    };
    assert(send_response(1, &response, false, NULL) == -1);
    assert(captured_length < sizeof(captured));
    captured[captured_length] = 0;
    assert(strstr((char *)captured, "HTTP/1.1 200 OK\r\n"));
    assert(!strstr((char *)captured, "HTTP/1.1 500"));
    reset(5);
    volatile sig_atomic_t running = 0;
    const struct io_context cancelled = {&running, NULL};
    assert(send_file_body(1, 2, 5, &cancelled) == -1 && errno == ECANCELED && position == 0);
    char path[] = "/tmp/web-stream-XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0 && write(file, "abcde", 5) == 5);
    struct stat info;
    assert(fstat(file, &info) == 0 && info.st_size == 5);
    assert(ftruncate(file, 3) == 0 && lseek(file, 0, SEEK_SET) == 0);
    reset(0);
    real_file_read = true;
    assert(send_file_body(1, file, (uintmax_t)info.st_size, NULL) == -1);
    assert(errno == EIO && captured_length == 3 && memcmp(captured, "abc", 3) == 0);
    assert(lseek(file, 0, SEEK_SET) == 0 && write(file, "abcdefg", 7) == 7);
    assert(lseek(file, 0, SEEK_SET) == 0);
    reset(0);
    real_file_read = true;
    assert(send_file_body(1, file, (uintmax_t)info.st_size, NULL) == 0);
    assert(captured_length == 5 && memcmp(captured, "abcde", 5) == 0);
    assert(close(file) == 0 && unlink(path) == 0);
    puts("[PASS] file sender short I/O, EINTR, real truncate/growth and cancellation checks");
    return 0;
}