#define _GNU_SOURCE
#include "handlers.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CGI_HEADER_MAX_BYTES 8192
#define CGI_READ_TIMEOUT_MS 5000
#define CGI_CHUNK_BUFFER_SIZE 16384

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

static bool is_token_char_cgi(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Find earliest CGI header terminator (\n\n or \r\n\r\n) and set sep_len, or SIZE_MAX. */
static size_t find_header_end(const unsigned char *buf, size_t len, size_t *sep_len) {
    for (size_t i = 0; i < len; ++i) {
        if (i + 2 <= len && buf[i] == '\n' && buf[i + 1] == '\n') {
            *sep_len = 2;
            return i;
        }
        if (i + 4 <= len && buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *sep_len = 4;
            return i;
        }
    }
    return SIZE_MAX;
}

/* Parse CGI Status field (200..599 excluding 204/304). Return false on invalid code. */
static bool parse_cgi_status(const char *val, size_t len, int *status_out) {
    if (len < 3) return false;
    for (size_t i = 0; i < 3; ++i) {
        if (val[i] < '0' || val[i] > '9') return false;
    }
    if (len > 3 && val[3] != ' ' && val[3] != '\t') return false;
    int code = (val[0] - '0') * 100 + (val[1] - '0') * 10 + (val[2] - '0');
    if (code < 200 || code > 599 || code == 204 || code == 304) return false;
    *status_out = code;
    return true;
}

static bool header_name_equal(const char *name, size_t len, const char *target) {
    return len == strlen(target) && strncasecmp(name, target, len) == 0;
}

/* Parse CGI response headers; require Content-Type, accept optional Status, ignore framing headers. */
static bool parse_cgi_headers(const unsigned char *hdr, size_t hdr_len,
                              struct cgi_body *cgi, int *status_out) {
    int status = 200;
    bool seen_type = false, seen_status = false;
    size_t pos = 0;
    while (pos < hdr_len) {
        size_t line_end = pos;
        while (line_end < hdr_len && hdr[line_end] != '\n') ++line_end;
        size_t content_end = line_end;
        if (content_end > pos && hdr[content_end - 1] == '\r') --content_end;
        if (content_end == pos) return false;

        const unsigned char *line = hdr + pos;
        size_t len = content_end - pos;
        size_t colon = 0;
        while (colon < len && line[colon] != ':') {
            if (!is_token_char_cgi(line[colon])) return false;
            ++colon;
        }
        if (colon == 0 || colon == len) return false;

        size_t vstart = colon + 1;
        while (vstart < len && (line[vstart] == ' ' || line[vstart] == '\t')) ++vstart;
        size_t vend = len;
        while (vend > vstart && (line[vend - 1] == ' ' || line[vend - 1] == '\t')) --vend;
        for (size_t i = vstart; i < vend; ++i) {
            unsigned char c = line[i];
            if ((c < 32 && c != '\t') || c == 127) return false;
        }

        const char *name = (const char *)line;
        const char *val = (const char *)(line + vstart);
        size_t vlen = vend - vstart;

        if (header_name_equal(name, colon, "content-type")) {
            if (seen_type || vlen == 0) return false;
            seen_type = true;
            cgi->content_type = malloc(vlen + 1);
            if (!cgi->content_type) return false;
            memcpy(cgi->content_type, val, vlen);
            cgi->content_type[vlen] = '\0';
        } else if (header_name_equal(name, colon, "status")) {
            if (seen_status || !parse_cgi_status(val, vlen, &status)) return false;
            seen_status = true;
        }

        pos = line_end < hdr_len ? line_end + 1 : hdr_len;
    }

    if (!seen_type) return false;
    *status_out = status;
    return true;
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
static int resolve_cgi_script(struct http_slice path, char **resolved_out,
                              char **script_name_out) {
    static const char prefix[] = "/cgi-bin/";
    size_t prefix_len = sizeof(prefix) - 1;
    *resolved_out = NULL;
    *script_name_out = NULL;

    if (memchr(path.data, '%', path.length)) return 400;
    if (path.length <= prefix_len) return 404;

    const unsigned char *rel = path.data + prefix_len;
    size_t rel_len = path.length - prefix_len;
    if (memchr(rel, '/', rel_len)) return 404;
    if ((rel_len == 1 && rel[0] == '.') ||
        (rel_len == 2 && rel[0] == '.' && rel[1] == '.')) {
        return 404;
    }

    char *root = realpath("cgi-bin", NULL);
    if (!root) return router_path_error_status(errno);

    size_t cand_len = (sizeof("cgi-bin/") - 1) + rel_len;
    char *candidate = malloc(cand_len + 1);
    if (!candidate) {
        free(root);
        return 500;
    }
    memcpy(candidate, "cgi-bin/", sizeof("cgi-bin/") - 1);
    memcpy(candidate + sizeof("cgi-bin/") - 1, rel, rel_len);
    candidate[cand_len] = '\0';

    char *resolved = realpath(candidate, NULL);
    free(candidate);
    if (!resolved) {
        int st = router_path_error_status(errno);
        free(root);
        return st;
    }
    if (!router_inside_directory(root, resolved)) {
        free(root);
        free(resolved);
        return 404;
    }
    free(root);

    struct stat info;
    if (stat(resolved, &info) < 0) {
        int st = router_path_error_status(errno);
        free(resolved);
        return st;
    }
    if (!S_ISREG(info.st_mode) || access(resolved, X_OK) < 0) {
        int st = S_ISREG(info.st_mode) ? router_path_error_status(errno) : 404;
        free(resolved);
        return st;
    }

    char *script_name = malloc(path.length + 1);
    if (!script_name) {
        free(resolved);
        return 500;
    }
    memcpy(script_name, path.data, path.length);
    script_name[path.length] = '\0';

    *resolved_out = resolved;
    *script_name_out = script_name;
    return 0;
}

/* Allocate a "KEY=VALUE" environment string for execve(). Return NULL on OOM/overflow. */
static char *make_env_var(const char *key, struct http_slice value) {
    size_t klen = strlen(key);
    if (value.length > SIZE_MAX - klen - 2) return NULL;
    char *entry = malloc(klen + 1 + value.length + 1);
    if (!entry) return NULL;
    memcpy(entry, key, klen);
    entry[klen] = '=';
    if (value.length > 0 && value.data) memcpy(entry + klen + 1, value.data, value.length);
    entry[klen + 1 + value.length] = '\0';
    return entry;
}

/* 멀티스레드에서 fork 이후 libc 상태를 건드리지 않도록 POSIX spawn을 사용한다.
 * 자식의 stdin/stdout, SIGPIPE 기본 동작, 관리자의 차단 신호 mask를 명시한다. */
static int spawn_cgi_process(const char *resolved, const char *script_name,
                             struct http_slice query, pid_t *pid_out, int *fd_out) {
    *pid_out = -1;
    *fd_out = -1;

    char *exec_arg = malloc(strlen(resolved) + 1);
    char *env_method = make_env_var("REQUEST_METHOD", (struct http_slice){(const unsigned char *)"GET", 3});
    char *env_query = make_env_var("QUERY_STRING", query);
    char *env_script = make_env_var("SCRIPT_NAME", (struct http_slice){(const unsigned char *)script_name, strlen(script_name)});
    char *env_proto = make_env_var("SERVER_PROTOCOL", (struct http_slice){(const unsigned char *)"HTTP/1.1", 8});
    char *env_path = make_env_var("PATH", (struct http_slice){(const unsigned char *)"/usr/local/bin:/usr/bin:/bin", 28});
    if (!exec_arg || !env_method || !env_query || !env_script || !env_proto || !env_path) {
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }
    memcpy(exec_arg, resolved, strlen(resolved) + 1);
    char *argv[] = {exec_arg, NULL};
    char *envp[] = {env_method, env_query, env_script, env_proto, env_path, NULL};

    int pipefd[2];
#ifdef __linux__
    int pipe_result = pipe2(pipefd, O_CLOEXEC);
#else
    int pipe_result = pipe(pipefd);
    if (pipe_result == 0 &&
        (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 ||
         fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0)) {
        close(pipefd[0]); close(pipefd[1]); pipe_result = -1;
    }
#endif
    if (pipe_result < 0) {
        free(exec_arg); free(env_method); free(env_query);
        free(env_script); free(env_proto); free(env_path);
        return 500;
    }

    /* 표준 FD가 닫혀 시작된 경우에도 file actions가 pipe를 잘못 닫지 않도록 3 이상으로 옮긴다. */
    int spawn_error = 0;
    for (size_t i = 0; i < 2; ++i) {
        if (pipefd[i] >= 3) continue;
        int duplicate = fcntl(pipefd[i], F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) { spawn_error = errno; break; }
        close(pipefd[i]);
        pipefd[i] = duplicate;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    bool actions_ready = false, attributes_ready = false;
    pid_t pid = -1;
    if (!spawn_error) {
        spawn_error = posix_spawn_file_actions_init(&actions);
        actions_ready = !spawn_error;
    }
    if (!spawn_error) {
        spawn_error = posix_spawnattr_init(&attributes);
        attributes_ready = !spawn_error;
    }
    sigset_t empty_mask, defaults;
    sigemptyset(&empty_mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    if (!spawn_error) spawn_error = posix_spawnattr_setsigmask(&attributes, &empty_mask);
    if (!spawn_error) spawn_error = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (!spawn_error) spawn_error = posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!spawn_error) spawn_error = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (!spawn_error) spawn_error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!spawn_error) spawn_error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (!spawn_error) spawn_error = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    if (!spawn_error) spawn_error = posix_spawn(&pid, resolved, &actions, &attributes, argv, envp);
    if (attributes_ready) posix_spawnattr_destroy(&attributes);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    free(exec_arg); free(env_method); free(env_query);
    free(env_script); free(env_proto); free(env_path);

    if (spawn_error) { close(pipefd[0]); errno = spawn_error; return 500; }
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
        size_t end = find_header_end(raw, used, &sep_len);
        if (end != SIZE_MAX) {
            if (!parse_cgi_headers(raw, end, cgi, status_out)) return 502;

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
    int err = resolve_cgi_script(request->path, &resolved, &script_name);
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
