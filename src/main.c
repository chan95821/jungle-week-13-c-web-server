#define _POSIX_C_SOURCE 200809L
#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Process cancellation only; no per-connection state is global. */
static volatile sig_atomic_t g_running = 1;

static void on_shutdown_signal(int sig);
static int setup_signals(void);

/* Read configuration, open one listener, then handle one request at a time. */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <proxy.conf>\n", argv[0]);
        return 1;
    }
    uint16_t port;
    if (parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    struct proxy_config proxy_config;
    struct proxy_selection_state selection = {0};
    if (proxy_load_config(argv[2], &proxy_config) < 0) {
        perror("proxy configuration");
        return 1;
    }

    struct stat root;
    if (stat("public", &root) < 0) {
        perror("public");
        return 1;
    }
    if (!S_ISDIR(root.st_mode)) {
        fprintf(stderr, "public: not a directory\n");
        return 1;
    }

    if (setup_signals() < 0) { perror("sigaction"); return 1; }
    int listener_fd = open_listenfd(port);
    if (listener_fd < 0) return 1;

    int exit_status = 0;
    while (g_running) {
        int client_fd = accept(listener_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) continue;
            perror("accept");
            exit_status = 1;
            break;
        }

        struct timespec accepted_at;
        if (clock_gettime(CLOCK_MONOTONIC, &accepted_at) < 0) {
            perror("clock_gettime");
            close(client_fd);
            exit_status = 1;
            break;
        }

        if (g_running) handle_client(client_fd, &g_running, &accepted_at,
                                     &proxy_config, &selection);
        /* Linux: do not retry close after EINTR; the FD may be released. */
        if (close(client_fd) < 0) perror("close client");
    }

    if (close(listener_fd) < 0) { perror("close listener"); exit_status = 1; }
    return exit_status;
}

/* Signal handler: clear global shutdown flag asynchronously without blocking. */
static void on_shutdown_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* Install SIGINT/SIGTERM handlers and ignore SIGPIPE. Return 0 or -1. */
static int setup_signals(void) {
    struct sigaction stop = {0};
    stop.sa_handler = on_shutdown_signal;
    if (sigemptyset(&stop.sa_mask) < 0 ||
        sigaction(SIGINT, &stop, NULL) < 0 ||
        sigaction(SIGTERM, &stop, NULL) < 0) return -1;
    stop.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &stop, NULL);
}
