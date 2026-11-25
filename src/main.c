#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "process_http_server.h"

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


/* Validate startup, own one listener, then run the parent and fixed workers. */
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

    struct process_signal_wait signals={0};
    if(prepare_process_signal_wait(&signals,&g_running,on_shutdown_signal)<0){perror("prepare signals");return 1;}
    int listener_fd=open_listenfd(port);
    if(listener_fd<0){restore_process_signal_wait(&signals);return 1;}
    int exit_status=run_process_http_server(listener_fd,&proxy_config,&signals);
    if (close(listener_fd) < 0) { perror("close listener"); exit_status = 1; }
    if(restore_process_signal_wait(&signals)<0){perror("restore signals");exit_status=1;}
    return exit_status;
}

/* The handler only updates the process-local control flag. */
static void on_shutdown_signal(int sig){record_process_signal(&g_running,sig);}
