#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "thread_pool.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Lock-free atomic cancellation flag safe across signal handlers and worker threads. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "atomic_int must be lock-free for signal safety");
static atomic_int g_running = 1;
static volatile sig_atomic_t g_stop_write_fd = -1;

struct client_task_context {
    const struct cancel_signal *cancel;
    const struct proxy_config *proxy_config;
    struct proxy_selection_state *selection;
};

static int parse_worker_count(const char *arg, size_t *out_count);
static int set_nonblocking(int fd);
static int setup_stop_pipe(int stop_pipe[2]);
static void close_stop_pipe(int stop_pipe[2]);
static void on_shutdown_signal(int sig);
static int setup_signals(void);
static void reset_shutdown_signals(void);
static void run_client_task(int client_fd, const struct timespec *accepted_at,
                            void *context);
static int run_accept_loop(int listener_fd, int stop_read_fd,
                           struct thread_pool *pool);

/* Read configuration, initialize thread pool, and dispatch accepted sockets. */
int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <port> <proxy.conf> [workers]\n", argv[0]);
        return 1;
    }

    uint16_t port;
    if (parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    size_t worker_count = THREAD_POOL_DEFAULT_WORKERS;
    if (argc == 4 && parse_worker_count(argv[3], &worker_count) < 0) {
        fprintf(stderr, "Invalid worker count (1..%d): %s\n",
                THREAD_POOL_MAX_WORKERS, argv[3]);
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

    struct proxy_selection_state selection;
    if (proxy_selection_init(&selection) < 0) {
        perror("proxy selection init");
        return 1;
    }

    int exit_status = 1;
    int stop_pipe[2] = {-1, -1};
    int listener_fd = -1;
    struct thread_pool pool = {0};

    if (setup_stop_pipe(stop_pipe) < 0) {
        perror("stop pipe");
        goto cleanup;
    }

    /* Publish write end before installing handlers so any thread can wake poll(). */
    g_stop_write_fd = stop_pipe[1];
    if (setup_signals() < 0) {
        perror("sigaction");
        goto cleanup;
    }

    listener_fd = open_listenfd(port);
    if (listener_fd < 0) {
        goto cleanup;
    }

    struct cancel_signal cancel = {
        .running = &g_running,
        .stop_fd = stop_pipe[0]
    };
    struct client_task_context task_context = {
        .cancel = &cancel,
        .proxy_config = &proxy_config,
        .selection = &selection
    };
    if (thread_pool_init(&pool, worker_count, THREAD_POOL_QUEUE_CAPACITY,
                         &g_running, run_client_task, &task_context) < 0) {
        perror("thread_pool_init");
        goto cleanup;
    }

    exit_status = run_accept_loop(listener_fd, stop_pipe[0], &pool);

cleanup:
    thread_pool_shutdown(&pool);
    if (listener_fd >= 0 && close(listener_fd) < 0) {
        perror("close listener");
        exit_status = 1;
    }
    reset_shutdown_signals();
    g_stop_write_fd = -1;
    close_stop_pipe(stop_pipe);
    proxy_selection_destroy(&selection);
    return exit_status;
}

/* Parse decimal worker count within [1, THREAD_POOL_MAX_WORKERS]. Return 0 or -1. */
static int parse_worker_count(const char *arg, size_t *out_count) {
    if (!arg || !out_count || !isdigit((unsigned char)arg[0])) return -1;

    errno = 0;
    char *endptr;
    unsigned long value = strtoul(arg, &endptr, 10);
    if (errno == ERANGE || endptr == arg || *endptr != '\0' ||
        value < 1 || value > THREAD_POOL_MAX_WORKERS) {
        return -1;
    }

    *out_count = (size_t)value;
    return 0;
}

/* Enable O_NONBLOCK on fd so pipe reads/writes never block signal or cleanup paths. */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Create self-pipe with FD_CLOEXEC and O_NONBLOCK on both ends. Return 0 or -1. */
static int setup_stop_pipe(int stop_pipe[2]) {
    if (pipe(stop_pipe) < 0) return -1;

    if (set_close_on_exec(stop_pipe[0]) < 0 ||
        set_close_on_exec(stop_pipe[1]) < 0 ||
        set_nonblocking(stop_pipe[0]) < 0 ||
        set_nonblocking(stop_pipe[1]) < 0) {
        close_stop_pipe(stop_pipe);
        return -1;
    }

    return 0;
}

/* Close any open ends of stop_pipe and reset descriptors to -1. */
static void close_stop_pipe(int stop_pipe[2]) {
    if (stop_pipe[0] >= 0) {
        close(stop_pipe[0]);
        stop_pipe[0] = -1;
    }
    if (stop_pipe[1] >= 0) {
        close(stop_pipe[1]);
        stop_pipe[1] = -1;
    }
}

/* Adapter bridging thread_pool callback signature to handle_client(). */
static void run_client_task(int client_fd, const struct timespec *accepted_at,
                            void *context) {
    struct client_task_context *task = context;
    handle_client(client_fd, task->cancel, accepted_at,
                  task->proxy_config, task->selection);
}

/* Watch listener_fd and stop_pipe[0] with poll(), accepting and submitting work. */
static int run_accept_loop(int listener_fd, int stop_read_fd,
                           struct thread_pool *pool) {
    while (g_running) {
        struct pollfd fds[2] = {
            {.fd = listener_fd, .events = POLLIN},
            {.fd = stop_read_fd, .events = POLLIN}
        };
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            return 1;
        }
        if ((fds[1].revents & (POLLIN | POLLERR | POLLHUP)) || !g_running) {
            break;
        }
        if (!(fds[0].revents & POLLIN)) continue;

        int client_fd = accept(listener_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            return 1;
        }

        struct timespec accepted_at;
        if (clock_gettime(CLOCK_MONOTONIC, &accepted_at) < 0) {
            perror("clock_gettime");
            close(client_fd);
            return 1;
        }

        if (thread_pool_submit(pool, client_fd, &accepted_at) < 0) {
            int saved_errno = errno;
            /* Linux: do not retry close after EINTR; the FD may be released. */
            if (close(client_fd) < 0) perror("close client");
            if (!g_running || saved_errno == ECANCELED) break;
            errno = saved_errno;
            perror("thread_pool_submit");
            return 1;
        }
    }

    return 0;
}

/* Signal handler: clear flag and wake poll() via async-signal-safe pipe write. */
static void on_shutdown_signal(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_running = 0;
    if (g_stop_write_fd >= 0) {
        static const unsigned char shutdown_wake_byte = 1;
        ssize_t ignored = write((int)g_stop_write_fd, &shutdown_wake_byte,
                                sizeof(shutdown_wake_byte));
        (void)ignored;
    }
    errno = saved_errno;
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

/* Restore default SIGINT/SIGTERM before closing stop_pipe to avoid late writes. */
static void reset_shutdown_signals(void) {
    struct sigaction dfl = {0};
    dfl.sa_handler = SIG_DFL;
    if (sigemptyset(&dfl.sa_mask) == 0) {
        sigaction(SIGINT, &dfl, NULL);
        sigaction(SIGTERM, &dfl, NULL);
    }
}
