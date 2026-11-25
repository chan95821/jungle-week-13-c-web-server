#define _XOPEN_SOURCE 700
#include "handlers.h"
#include "cgi_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


struct cgi_body {
    int fd;
    pid_t pid;
    bool reaped;
    bool eof_reached;
    char *content_type;
    size_t initial_pos;
    size_t initial_len;
    unsigned char buffer[CGI_CHUNK_BUFFER_SIZE];
};

/* Compute monotonic clock deadline timeout_ms milliseconds ahead. */
static bool make_deadline(struct timespec *deadline, int timeout_ms) {
    if (clock_gettime(CLOCK_MONOTONIC, deadline) < 0) return false;
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return true;
}

/* Wait via poll() until fd is readable or deadline expires. Return 0 or -1 (ETIMEDOUT). */
static int wait_readable(int fd, const struct timespec *deadline) {
    for (;;) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
        long sec = (long)(deadline->tv_sec - now.tv_sec);
        long nsec = deadline->tv_nsec - now.tv_nsec;
        long long remaining_ms = (long long)sec * 1000LL + (nsec + 999999L) / 1000000L;
        if (remaining_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        int timeout = remaining_ms > INT32_MAX ? INT32_MAX : (int)remaining_ms;
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        return 0;
    }
}

/* Reap CGI child via waitpid(); on force_terminate send SIGTERM then SIGKILL to prevent zombies. */
static int reap_cgi_child(struct cgi_body *cgi, bool force_terminate) {
    if (cgi->reaped || cgi->pid <= 0) return -1;

    int wstatus = 0;
    if (!force_terminate) {
        pid_t res;
        do {
            res = waitpid(cgi->pid, &wstatus, 0);
        } while (res < 0 && errno == EINTR);
        if (res < 0) return -1;
        cgi->reaped = true;
        return wstatus;
    }

    pid_t res = waitpid(cgi->pid, &wstatus, WNOHANG);
    if (res == 0) {
        kill(cgi->pid, SIGTERM);
        for (int attempt = 0; attempt < 20; ++attempt) {
            struct timespec pause_ts = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&pause_ts, NULL);
            res = waitpid(cgi->pid, &wstatus, WNOHANG);
            if (res != 0) break;
        }
        if (res == 0) {
            kill(cgi->pid, SIGKILL);
            do {
                res = waitpid(cgi->pid, &wstatus, 0);
            } while (res < 0 && errno == EINTR);
        }
    } else if (res < 0 && errno == EINTR) {
        do {
            res = waitpid(cgi->pid, &wstatus, 0);
        } while (res < 0 && errno == EINTR);
    }

    if (res > 0) cgi->reaped = true;
    return wstatus;
}

/* Yield buffered initial body first, then stream pipe reads; require exit status 0 at EOF. */
static enum body_result cgi_body_next(void *state, struct http_slice *chunk) {
    struct cgi_body *cgi = state;
    if (cgi->initial_pos < cgi->initial_len) {
        *chunk = (struct http_slice){
            cgi->buffer + cgi->initial_pos,
            cgi->initial_len - cgi->initial_pos
        };
        cgi->initial_pos = cgi->initial_len;
        return BODY_DATA;
    }
    if (cgi->eof_reached) return BODY_END;

    struct timespec deadline;
    if (!make_deadline(&deadline, CGI_READ_TIMEOUT_MS)) return BODY_ERROR;
    if (wait_readable(cgi->fd, &deadline) < 0) return BODY_ERROR;

    ssize_t count;
    do {
        count = read(cgi->fd, cgi->buffer, sizeof(cgi->buffer));
    } while (count < 0 && errno == EINTR);
    if (count < 0) return BODY_ERROR;
    if (count > 0) {
        *chunk = (struct http_slice){cgi->buffer, (size_t)count};
        return BODY_DATA;
    }

    int wstatus = reap_cgi_child(cgi, false);
    if (wstatus < 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        errno = EIO;
        return BODY_ERROR;
    }
    cgi->eof_reached = true;
    return BODY_END;
}

/* Close CGI pipe FD, reap any running child process, and free cgi_body state. */
static void cgi_body_cleanup(void *state) {
    struct cgi_body *cgi = state;
    if (cgi->fd >= 0) {
        close(cgi->fd);
        cgi->fd = -1;
    }
    if (!cgi->reaped && cgi->pid > 0) reap_cgi_child(cgi, true);
    free(cgi->content_type);
    free(cgi);
}

/* Validate single-depth CGI script under "cgi-bin": reject '%' (400), outside/non-exec (404). */
/* Fork CGI child with minimal envp and pipe stdout; parent closes write end immediately. */
static int spawn_cgi_process(const char *resolved, const char *script_name,
                             struct http_slice query, pid_t *pid_out, int *fd_out) {
    *pid_out = -1;
    *fd_out = -1;

    char *exec_arg = malloc(strlen(resolved) + 1);
    char *env_method = cgi_make_env_var("REQUEST_METHOD", (struct http_slice){(const unsigned char *)"GET", 3});
    char *env_query = cgi_make_env_var("QUERY_STRING", query);
    char *env_script = cgi_make_env_var("SCRIPT_NAME", (struct http_slice){(const unsigned char *)script_name, strlen(script_name)});
    char *env_proto = cgi_make_env_var("SERVER_PROTOCOL", (struct http_slice){(const unsigned char *)"HTTP/1.1", 8});
    char *env_path = cgi_make_env_var("PATH", (struct http_slice){(const unsigned char *)"/usr/local/bin:/usr/bin:/bin", 28});
    if (!exec_arg || !env_method || !env_query || !env_script || !env_proto || !env_path) {
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }
    memcpy(exec_arg, resolved, strlen(resolved) + 1);
    char *argv[] = {exec_arg, NULL};
    char *envp[] = {env_method, env_query, env_script, env_proto, env_path, NULL};

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        close(pipefd[0]);

        int devnull = open("/dev/null", O_RDONLY);
        if (devnull < 0) _exit(127);
        if (devnull != STDIN_FILENO) {
            if (dup2(devnull, STDIN_FILENO) < 0) _exit(127);
            close(devnull);
        }

        if (pipefd[1] != STDOUT_FILENO) {
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
            close(pipefd[1]);
        }

        execve(resolved, argv, envp);
        _exit(127);
    }

    close(pipefd[1]);
    free(exec_arg); free(env_method); free(env_query);
    free(env_script); free(env_proto); free(env_path);

    *pid_out = pid;
    *fd_out = pipefd[0];
    return 0;
}

/* Read CGI stdout up to header terminator within timeout, saving any trailing body bytes. */
static int read_cgi_header(struct cgi_body *cgi, int *status_out) {
    unsigned char raw[CGI_HEADER_MAX_BYTES];
    size_t used = 0;
    struct timespec deadline;
    if (!make_deadline(&deadline, CGI_READ_TIMEOUT_MS)) return 500;

    for (;;) {
        size_t sep_len = 0;
        size_t end = cgi_find_header_end(raw, used, &sep_len);
        if (end != SIZE_MAX) {
            if (!cgi_parse_headers(raw, end, &cgi->content_type, status_out)) return 502;

            size_t body_start = end + sep_len;
            size_t rem = used - body_start;
            if (rem > 0) {
                memcpy(cgi->buffer, raw + body_start, rem);
                cgi->initial_pos = 0;
                cgi->initial_len = rem;
            }
            return 0;
        }

        if (used == sizeof(raw)) return 502;
        if (wait_readable(cgi->fd, &deadline) < 0) return 502;

        ssize_t count;
        do {
            count = read(cgi->fd, raw + used, sizeof(raw) - used);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) return 502;
        used += (size_t)count;
    }
}

/* Execute CGI script under /cgi-bin/ and prepare chunked response (500 on OS fail, 502 on CGI fail). */
void prepare_cgi(const struct http_request *request,
                 struct router_response *response) {
    char *resolved = NULL, *script_name = NULL;
    int err = cgi_resolve_script(request->path, &resolved, &script_name);
    if (err != 0) {
        router_prepare_error(response, err, NULL);
        return;
    }

    struct cgi_body *cgi = calloc(1, sizeof(*cgi));
    if (!cgi) {
        free(resolved);
        free(script_name);
        router_prepare_error(response, 500, NULL);
        return;
    }
    cgi->fd = -1;
    cgi->pid = -1;

    err = spawn_cgi_process(resolved, script_name, request->query, &cgi->pid, &cgi->fd);
    free(resolved);
    free(script_name);
    if (err != 0) {
        cgi_body_cleanup(cgi);
        router_prepare_error(response, err, NULL);
        return;
    }

    int http_status = 200;
    err = read_cgi_header(cgi, &http_status);
    if (err != 0) {
        cgi_body_cleanup(cgi);
        router_prepare_error(response, err, NULL);
        return;
    }

    *response = (struct router_response){
        .status = http_status,
        .content_type = cgi->content_type,
        .chunked = true,
        .body_provider = {
            .next_func = cgi_body_next,
            .cleanup_func = cgi_body_cleanup,
            .state = cgi
        }
    };
}
