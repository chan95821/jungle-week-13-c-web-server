#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* Experimental starting values, not protocol requirements or optima. */
enum {
    HEADER_LIMIT_BYTES = 8 * 1024,
    LISTEN_BACKLOG = 128,
    IO_TIMEOUT_SECONDS = 5
};

/* Process shutdown state only; connection state stays in local variables. */
static volatile sig_atomic_t stop_requested = 0;

enum io_result {
    IO_OK,
    IO_PEER_CLOSED,
    IO_TIMEOUT,
    IO_TOO_LARGE,
    IO_STOPPED,
    IO_ERROR
};

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction action = {0};
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        return -1;
    }

    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL);
}

static int parse_port(const char *text, unsigned short *port)
{
    if (*text == '\0') {
        return -1;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || value == 0 || value > 65535) {
        return -1;
    }
    *port = (unsigned short)value;
    return 0;
}

static int set_io_timeouts(int fd)
{
    const struct timeval timeout = {
        .tv_sec = IO_TIMEOUT_SECONDS,
        .tv_usec = 0
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) == -1) {
        return -1;
    }
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                      &timeout, sizeof(timeout));
}

static void close_socket(int fd)
{
    /* On Linux, do not retry close(): the descriptor may already be released. */
    if (close(fd) == -1) {
        perror("close");
    }
}

static enum io_result receive_headers(int fd)
{
    unsigned char buffer[HEADER_LIMIT_BYTES];
    size_t used = 0;

    while (!stop_requested) {
        size_t previous_used = used;
        ssize_t received = recv(fd, buffer + used, sizeof(buffer) - used, 0);
        if (stop_requested) {
            return IO_STOPPED;
        }
        if (received == 0) {
            return IO_PEER_CLOSED;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return IO_TIMEOUT;
            }
            return IO_ERROR;
        }

        used += (size_t)received;
        /* Recheck the three-byte overlap when CRLFCRLF crosses recv calls. */
        size_t start = previous_used > 3 ? previous_used - 3 : 0;
        for (size_t i = start; i + 4 <= used; ++i) {
            if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) {
                return IO_OK;
            }
        }
        /* A complete header ending exactly at the limit is accepted above. */
        if (used == sizeof(buffer)) {
            return IO_TOO_LARGE;
        }
    }
    return IO_STOPPED;
}

static enum io_result send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length && !stop_requested) {
        ssize_t count = send(fd, data + sent, length - sent, 0);
        if (stop_requested) {
            return IO_STOPPED;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return IO_TIMEOUT;
            }
            return IO_ERROR;
        }
        if (count == 0) {
            errno = EIO;
            return IO_ERROR;
        }
        sent += (size_t)count;
    }
    return stop_requested ? IO_STOPPED : IO_OK;
}

static enum io_result send_response(int fd, const char *status, const char *body)
{
    char header[256];
    size_t body_length = strlen(body);
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 %s\r\n"
                          "Content-Type: text/plain; charset=utf-8\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n", status, body_length);
    if (length < 0 || (size_t)length >= sizeof(header)) {
        errno = EOVERFLOW;
        return IO_ERROR;
    }

    enum io_result result = send_all(fd, header, (size_t)length);
    return result == IO_OK ? send_all(fd, body, body_length) : result;
}

/* Borrow fd from main; main closes it exactly once after this function. */
static void handle_connection(int fd)
{
    if (set_io_timeouts(fd) == -1) {
        perror("client socket timeout setup");
        return;
    }

    enum io_result result = receive_headers(fd);
    switch (result) {
    case IO_OK:
        result = send_response(fd, "200 OK", "Hello, world!\n");
        break;
    case IO_TOO_LARGE:
        result = send_response(fd, "431 Request Header Fields Too Large",
                               "Request headers too large.\n");
        break;
    case IO_PEER_CLOSED:
        fputs("client closed before complete headers\n", stderr);
        return;
    case IO_TIMEOUT:
        fputs("client header receive call timed out\n", stderr);
        return;
    case IO_STOPPED:
        return;
    case IO_ERROR:
        perror("recv");
        return;
    }

    if (result == IO_TIMEOUT) {
        fputs("client response send call timed out\n", stderr);
    } else if (result == IO_ERROR) {
        perror("send response");
    }
}

int main(int argc, char **argv)
{
    unsigned short port;
    if (argc != 2 || parse_port(argv[1], &port) == -1) {
        fprintf(stderr, "Usage: %s <port: 1-65535, decimal digits only>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() == -1) {
        perror("signal setup");
        return EXIT_FAILURE;
    }

    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int exit_status = EXIT_FAILURE;
    const int reuse_address = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("SO_REUSEADDR");
        goto cleanup;
    }
    /* Also bounds accept() if a signal lands just before its blocking call. */
/*    if (set_io_timeouts(listener) == -1) {
        perror("listener timeout setup");
        goto cleanup;
    }*/

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)}
    };
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        goto cleanup;
    }
    if (listen(listener, LISTEN_BACKLOG) == -1) {
        perror("listen");
        goto cleanup;
    }

    printf("Listening on 0.0.0.0:%u\n", (unsigned int)port);
    fflush(stdout);
    while (!stop_requested) {
        int client = accept(listener, NULL, NULL);
        if (client == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            goto cleanup;
        }
        if (!stop_requested) {
            handle_connection(client);
        }
        close_socket(client);
    }
    exit_status = EXIT_SUCCESS;

cleanup:
    close_socket(listener);
    return exit_status;
}
