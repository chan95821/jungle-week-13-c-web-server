#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "event_server.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int setup_signals(void);

/* 설정 → listener 하나 → 고정 worker/epoll → 종료/join. 연결 처리 자체는 worker가 소유한다. */
int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <port> <proxy.conf> [workers:1..64]\n", argv[0]);
        return 1;
    }
    uint16_t port;
    if (parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    struct proxy_config proxy_config;
    uint16_t worker_count = EVENT_DEFAULT_WORKERS;
    if (argc == 4 && (parse_port(argv[3], &worker_count) < 0 || worker_count > EVENT_MAX_WORKERS)) {
        fprintf(stderr, "Invalid worker count: %s\n", argv[3]);
        return 1;
    }
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

    int exit_status = run_event_server(listener_fd, &proxy_config, worker_count) < 0;
    if (exit_status) perror("event server");

    if (close(listener_fd) < 0) { perror("close listener"); exit_status = 1; }
    return exit_status;
}

/* SIGINT/TERM은 run_event_server의 signalfd로 처리한다. SIGPIPE만 프로세스 전체에서 무시. */
static int setup_signals(void) {
    struct sigaction stop = {0};
    stop.sa_handler = SIG_IGN;
    if (sigemptyset(&stop.sa_mask) < 0) return -1;
    return sigaction(SIGPIPE, &stop, NULL);
}
