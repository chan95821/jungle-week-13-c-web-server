#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

static volatile sig_atomic_t g_running = 1;

static void on_shutdown_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int setup_signals(void) {
    struct sigaction sa_stop;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return -1;
    }

    sa_stop.sa_handler = on_shutdown_signal;
    sigemptyset(&sa_stop.sa_mask);
    sa_stop.sa_flags = 0;

    if (sigaction(SIGINT, &sa_stop, NULL) < 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa_stop, NULL) < 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    uint16_t port = 0;
    if (parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    if (setup_signals() < 0) {
        perror("sigaction");
        return 1;
    }

    int listenfd = open_listenfd(port);
    if (listenfd < 0) {
        return 1;
    }

    while (g_running) {
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        handle_client(connfd);
        close(connfd);
    }

    close(listenfd);
    return 0;
}
