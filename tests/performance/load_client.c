#define _POSIX_C_SOURCE 200809L
/* Benchmark client only: fixed HTTP/1.1 framing, new TCP connection per request. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

struct benchmark {
    int ports[2], concurrency, rate;
    bool post;
    double start, end;
    pthread_mutex_t lock;
    size_t scheduled, attempted, success, failure, status_5xx, io_errors, invalid;
    size_t backend[3], late_skipped;
    double *latencies;
    size_t capacity;
};
static double now_seconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) abort();
    return value.tv_sec + value.tv_nsec / 1e9;
}
static void sleep_until(double when) {
    struct timespec value = {(time_t)when, (long)((when - (time_t)when) * 1e9)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &value, NULL) == EINTR) {}
}
static int wait_socket(int fd, short events, double deadline) {
    for (;;) {
        double remaining = deadline - now_seconds();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd ready = {.fd = fd, .events = events};
        int result = poll(&ready, 1, (int)ceil(remaining * 1000));
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) errno = ETIMEDOUT;
        return result > 0 ? 0 : -1;
    }
}
static int send_request(int fd, const unsigned char *bytes, size_t length, double deadline) {
    size_t offset = 0;
    while (offset < length) {
        if (wait_socket(fd, POLLOUT, deadline) < 0) return -1;
        ssize_t count = send(fd, bytes + offset, length - offset, 0);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (count <= 0) return -1;
        offset += (size_t)count;
    }
    return 0;
}
/* 0 success; 1 invalid status/framing/body; -1 transport/timeout. */
static int perform_request(int port, bool post, int *status, int *backend) {
    double deadline = now_seconds() + 5;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int outcome = -1;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto done;
    struct sockaddr_in endpoint = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    if (connect(fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) < 0) {
        if (errno != EINPROGRESS || wait_socket(fd, POLLOUT, deadline) < 0) goto done;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) goto done;
    }
    unsigned char request[2048];
    int count = snprintf((char *)request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: benchmark.local\r\nConnection: close\r\n%s\r\n",
        post ? "POST" : "GET", post ? "/api/submissions" : "/api/submission-status",
        post ? "Content-Length: 1024\r\nContent-Type: application/octet-stream\r\n" : "");
    if (count < 0 || (size_t)count >= sizeof(request) - 1024) goto done;
    size_t request_length = (size_t)count;
    if (post) for (size_t i = 0; i < 1024; ++i) request[request_length++] = (unsigned char)i;
    if (send_request(fd, request, request_length, deadline) < 0 || shutdown(fd, SHUT_WR) < 0) goto done;
    unsigned char response[10 * 1024];
    size_t used = 0;
    for (;;) {
        if (used == sizeof(response) || wait_socket(fd, POLLIN, deadline) < 0) goto done;
        ssize_t received = recv(fd, response + used, sizeof(response) - used, 0);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (received < 0) goto done;
        if (received == 0) break;
        used += (size_t)received;
    }
    outcome = 1;
    size_t header_end = 0;
    for (size_t i = 0; i + 4 <= used; ++i)
        if (!memcmp(response + i, "\r\n\r\n", 4)) { header_end = i + 4; break; }
    if (!header_end || header_end > 8192) goto done;
    char header[8193];
    memcpy(header, response, header_end);
    header[header_end] = 0;
    if (sscanf(header, "HTTP/1.1 %d", status) != 1) goto done;
    size_t length = 0;
    bool has_length = false;
    char *line = strstr(header, "\r\n");
    while (line && line[2]) {
        line += 2;
        char *end = strstr(line, "\r\n");
        if (!end) break;
        *end = 0;
        if (!strncasecmp(line, "Content-Length: ", 16)) {
            if (has_length) goto done;
            char *tail;
            unsigned long value = strtoul(line + 16, &tail, 10);
            if (*tail) goto done;
            length = (size_t)value; has_length = true;
        }
        if (!strncasecmp(line, "X-Backend-ID: ", 14)) {
            const char *identity = line + 14;
            if (!strcmp(identity, "submission")) *backend = 0;
            else if (!strcmp(identity, "query-a")) *backend = 1;
            else if (!strcmp(identity, "query-b")) *backend = 2;
        }
        *end = '\r'; line = end;
    }
    if (*status != (post ? 201 : 200) || !has_length || length != 1024 || used - header_end != length ||
        (post ? *backend != 0 : (*backend != 1 && *backend != 2))) goto done;
    for (size_t i = 0; i < length; ++i)
        if (response[header_end + i] != (unsigned char)i) goto done;
    outcome = 0;
done:
    close(fd);
    return outcome;
}
static void *run_worker(void *argument) {
    struct benchmark *bench = argument;
    sleep_until(bench->start);
    for (;;) {
        pthread_mutex_lock(&bench->lock);
        size_t slot = bench->scheduled++;
        pthread_mutex_unlock(&bench->lock);
        double due = bench->rate ? bench->start + (double)slot / bench->rate : now_seconds();
        if (due >= bench->end) break;
        if (bench->rate) sleep_until(due);
        double started = now_seconds();
        if (started >= bench->end) {
            pthread_mutex_lock(&bench->lock); ++bench->late_skipped; pthread_mutex_unlock(&bench->lock);
            break;
        }
        int status = 0, backend = -1;
        int result = perform_request(bench->ports[slot % 2], bench->post, &status, &backend);
        double latency = (now_seconds() - started) * 1000;
        pthread_mutex_lock(&bench->lock);
        ++bench->attempted;
        if (result == 0) {
            if (bench->success == bench->capacity) {
                size_t capacity = bench->capacity ? bench->capacity * 2 : 4096;
                double *values = realloc(bench->latencies, capacity * sizeof(*values));
                if (!values) abort();
                bench->latencies = values; bench->capacity = capacity;
            }
            bench->latencies[bench->success++] = latency;
            ++bench->backend[backend];
        } else {
            ++bench->failure;
            if (result < 0) ++bench->io_errors; else ++bench->invalid;
            if (status >= 500 && status < 600) ++bench->status_5xx;
        }
        pthread_mutex_unlock(&bench->lock);
    }
    return NULL;
}
static int compare_double(const void *a, const void *b) {
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}
int main(int argc, char **argv) {
    if (argc != 7) { fprintf(stderr, "Usage: %s port alternate-port GET|POST rate concurrency seconds\n", argv[0]); return 2; }
    struct benchmark bench = {.lock = PTHREAD_MUTEX_INITIALIZER};
    bench.ports[0] = atoi(argv[1]); bench.ports[1] = atoi(argv[2]);
    bench.post = !strcmp(argv[3], "POST"); bench.rate = atoi(argv[4]); bench.concurrency = atoi(argv[5]);
    double duration = atof(argv[6]);
    if (bench.ports[0] < 1 || bench.ports[0] > 65535 || bench.ports[1] < 1 || bench.ports[1] > 65535 ||
        (strcmp(argv[3], "GET") && !bench.post) || bench.rate < 0 || bench.concurrency < 1 || bench.concurrency > 16 || duration <= 0) return 2;
    signal(SIGPIPE, SIG_IGN);
    bench.start = now_seconds() + 0.05; bench.end = bench.start + duration;
    pthread_t workers[16];
    for (int i = 0; i < bench.concurrency; ++i) if (pthread_create(&workers[i], NULL, run_worker, &bench)) abort();
    sleep_until(bench.end);
    for (int i = 0; i < bench.concurrency; ++i) pthread_join(workers[i], NULL);
    double elapsed = now_seconds() - bench.start;
    if (bench.success > 1) qsort(bench.latencies, bench.success, sizeof(double), compare_double);
    double p50 = bench.success ? bench.latencies[(size_t)ceil(bench.success * .5) - 1] : 0;
    double p95 = bench.success ? bench.latencies[(size_t)ceil(bench.success * .95) - 1] : 0;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) abort();
    printf("{\"cpu_user_s\":%.6f,\"cpu_system_s\":%.6f,\"max_rss_kib\":%ld,\"attempted\":%zu,\"success\":%zu,\"failures\":%zu,\"io_errors\":%zu,\"invalid_responses\":%zu,"
           "\"status_5xx\":%zu,\"late_skipped\":%zu,\"duration_s\":%.6f,\"success_rps\":%.3f,\"p50_ms\":%.6f,\"p95_ms\":%.6f,"
           "\"backend_hits\":{\"submission\":%zu,\"query-a\":%zu,\"query-b\":%zu}}\n",
           usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6,
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6, usage.ru_maxrss,
           bench.attempted, bench.success, bench.failure, bench.io_errors, bench.invalid, bench.status_5xx,
           bench.late_skipped, elapsed, bench.success / elapsed, p50, p95, bench.backend[0], bench.backend[1], bench.backend[2]);
    free(bench.latencies);
    pthread_mutex_destroy(&bench.lock);
    return bench.failure ? 1 : 0;
}