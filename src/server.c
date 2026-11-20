#include "server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LISTEN_BACKLOG 128

int parse_port(const char *arg, uint16_t *out_port) {
    char *endptr = NULL;
    long val = 0;

    if (arg == NULL || out_port == NULL || arg[0] == '\0') {
        return -1;
    }
    if (!isdigit((unsigned char)arg[0])) {
        return -1;
    }

    errno = 0;
    val = strtol(arg, &endptr, 10);
    if (errno == ERANGE || endptr == arg || *endptr != '\0') {
        return -1;
    }
    if (val < 1 || val > 65535) {
        return -1;
    }

    *out_port = (uint16_t)val;
    return 0;
}

int open_listenfd(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len) {
    size_t total = 0;

    if (buf == NULL || cap < 4) {
        return -1;
    }

    while (total < cap) {
        ssize_t n = recv(fd, buf + total, cap - total, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }

        size_t prev_total = total;
        total += (size_t)n;

        if (total >= 4) {
            size_t start = (prev_total >= 3) ? prev_total - 3 : 0;
            for (size_t i = start; i + 3 < total; i++) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    if (out_len != NULL) {
                        *out_len = total;
                    }
                    return 0;
                }
            }
        }
    }

    return -1;
}

int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t sent = 0;

    if (len == 0) {
        return 0;
    }
    if (buf == NULL) {
        return -1;
    }

    while (sent < len) {
        ssize_t n = send(fd, ptr + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

void handle_client(int connfd) {
    static const char FIXED_RESPONSE[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Hello";

    char buf[REQ_HEADER_MAX_BYTES];
    size_t req_len = 0;

    if (recv_until_header_end(connfd, buf, sizeof(buf), &req_len) < 0) {
        return;
    }

    (void)send_all(connfd, FIXED_RESPONSE, sizeof(FIXED_RESPONSE) - 1);
}
