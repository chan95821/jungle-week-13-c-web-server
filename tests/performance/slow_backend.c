#define _POSIX_C_SOURCE 200809L
/* Concurrent delay-injection backend for Iteration 5 HoL benchmark experiments. */
#include "../../src/http.h"
#include "../../src/socket_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_running = 1;

struct backend_config {
    const char *identity;
    long delay_ms;
};

struct conn_arg {
    int fd;
    const struct backend_config *cfg;
};

static void on_stop(int sig) {
    (void)sig;
    g_running = 0;
}

static void *handle_conn(void *arg) {
    struct conn_arg *conn = arg;
    int fd = conn->fd;
    const struct backend_config *cfg = conn->cfg;
    free(conn);

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == 0) {
        deadline.tv_sec += 5;
        struct cancel_signal cancel = {.running = &g_running, .stop_fd = -1};
        struct io_context io = {.cancel = &cancel, .deadline = &deadline};
        unsigned char buf[REQ_HEADER_MAX_BYTES];
        size_t used = 0;
        bool done = false;
        while (used < sizeof(buf) && !done) {
            ssize_t n = receive_with_deadline(fd, buf + used, sizeof(buf) - used, &io);
            if (n <= 0) break;
            used += (size_t)n;
            for (size_t i = 0; i + 4 <= used; ++i) {
                if (memcmp(buf + i, "\r\n\r\n", 4) == 0) { done = true; break; }
            }
        }
        if (done && g_running) {
            if (cfg->delay_ms > 0) {
                struct timespec req = {
                    .tv_sec = cfg->delay_ms / 1000,
                    .tv_nsec = (cfg->delay_ms % 1000) * 1000000L
                };
                nanosleep(&req, NULL);
            }
            if (g_running) {
                unsigned char resp[1536];
                int hlen = snprintf((char *)resp, sizeof(resp),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Content-Length: 1024\r\n"
                    "X-Backend-ID: %s\r\n"
                    "Connection: close\r\n\r\n",
                    cfg->identity);
                if (hlen > 0 && (size_t)hlen + 1024 <= sizeof(resp)) {
                    for (size_t i = 0; i < 1024; ++i) {
                        resp[(size_t)hlen + i] = (unsigned char)i;
                    }
                    (void)send_all_with_deadline(fd, resp, (size_t)hlen + 1024, &io);
                }
            }
        }
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    uint16_t port = 0;
    if (argc != 4 || parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Usage: %s <port> <identity> <delay_ms>\n", argv[0]);
        return 1;
    }
    long delay_ms = strtol(argv[3], NULL, 10);
    if (delay_ms < 0) delay_ms = 0;

    struct sigaction sa = {.sa_handler = on_stop};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    int listener = open_listenfd(port);
    if (listener < 0) return 1;
    struct backend_config cfg = {.identity = argv[2], .delay_ms = delay_ms};

    while (g_running) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) continue;
            break;
        }
        struct conn_arg *arg = malloc(sizeof(*arg));
        if (!arg) { close(client); continue; }
        arg->fd = client;
        arg->cfg = &cfg;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, handle_conn, arg) != 0) {
            free(arg);
            close(client);
        }
        pthread_attr_destroy(&attr);
    }

    close(listener);
    return 0;
}
