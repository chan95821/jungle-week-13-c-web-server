#define _POSIX_C_SOURCE 200809L

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

int recv_until_header_end(int fd, char *buf, size_t cap,
                          size_t *out_header_len, size_t *out_total_len) {
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
                if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                    if (out_header_len != NULL) {
                        *out_header_len = i + 4;
                    }
                    if (out_total_len != NULL) {
                        *out_total_len = total;
                    }
                    return 0;
                }
            }
        }
    }

    return -2;
}

int recv_body(int fd, const char *excess, size_t excess_len,
              size_t content_length, uint8_t **out_body, size_t *out_len) {
    if (out_body == NULL || out_len == NULL) {
        return -1;
    }
    *out_body = NULL;
    *out_len = 0;

    if (content_length == 0) {
        return 0;
    }

    uint8_t *buf = malloc(content_length);
    if (buf == NULL) {
        return -1;
    }

    size_t received = 0;
    if (excess != NULL && excess_len > 0) {
        size_t copy_len = (excess_len < content_length) ? excess_len : content_length;
        memcpy(buf, excess, copy_len);
        received = copy_len;
    }

    while (received < content_length) {
        ssize_t n = recv(fd, buf + received, content_length - received, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return -1;
        }
        if (n == 0) {
            free(buf);
            return -1;
        }
        received += (size_t)n;
    }

    *out_body = buf;
    *out_len = content_length;
    return 0;
}

int recv_http_request(int connfd, HttpRequest *req, HttpStatus *http_status) {
    char head_buf[REQ_HEADER_MAX_BYTES];
    size_t header_len = 0;
    size_t total_len = 0;

    if (req == NULL || http_status == NULL) {
        return -1;
    }
    *http_status = HTTP_STATUS_NONE;

    int rc = recv_until_header_end(connfd, head_buf, sizeof(head_buf),
                                   &header_len, &total_len);
    if (rc < 0) {
        if (rc == -2) {
            *http_status = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
        }
        return -1;
    }

    if (parse_request_head(head_buf, header_len, req, http_status) < 0) {
        return -1;
    }

    size_t excess_len = total_len - header_len;
    const char *excess_ptr = head_buf + header_len;
    if (recv_body(connfd, excess_ptr, excess_len, req->content_length,
                  &req->body, &req->body_len) < 0) {
        *http_status = HTTP_STATUS_NONE;
        return -1;
    }

    return 0;
}

int send_http_response(int connfd, const HttpResponse *resp) {
    char header_buf[512];
    int n = snprintf(header_buf, sizeof(header_buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     resp->status_code,
                     resp->reason,
                     resp->content_type,
                     resp->body_len);
    if (n < 0 || (size_t)n >= sizeof(header_buf)) {
        return -1;
    }

    if (send_all(connfd, header_buf, (size_t)n) < 0) {
        return -1;
    }
    if (resp->body_len > 0 && resp->body != NULL) {
        if (send_all(connfd, resp->body, resp->body_len) < 0) {
            return -1;
        }
    }
    return 0;
}

void free_http_request(HttpRequest *req) {
    if (req == NULL) {
        return;
    }
    free(req->body);
    req->body = NULL;
    req->body_len = 0;
}

void free_http_response(HttpResponse *resp) {
    if (resp == NULL) {
        return;
    }
    if (resp->owns_body && resp->body != NULL) {
        free((void *)resp->body);
    }
    resp->body = NULL;
    resp->body_len = 0;
    resp->owns_body = 0;
}

void handle_client(int connfd) {
    HttpRequest req;
    HttpResponse resp;
    HttpStatus http_status = HTTP_STATUS_NONE;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    if (recv_http_request(connfd, &req, &http_status) < 0) {
        if (http_status != HTTP_STATUS_NONE) {
            set_error_response(&resp, http_status);
            (void)send_http_response(connfd, &resp);
        }
        goto cleanup;
    }

    dispatch_request(&req, &resp);
    (void)send_http_response(connfd, &resp);

cleanup:
    free_http_response(&resp);
    free_http_request(&req);
}
