#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "router.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int receive_request_headers(int fd, unsigned char *buf, size_t cap,
                          size_t *received, size_t *header_end,
                          const struct io_context *io);
static int send_prepared_response(int fd, struct router_response *response,
                                  bool head_request, const struct io_context *io);
static void finish_response(int fd, const struct io_context *io);

void handle_client(int client_fd, const volatile sig_atomic_t *running,
                   const struct timespec *accepted_at,
                   const struct proxy_config *proxy_config,
                   struct proxy_selection_state *selection) {
    unsigned char request_header[REQ_HEADER_MAX_BYTES];
    unsigned char *request_body = NULL;
    struct http_request request = {0};
    struct request_route selected_route = {.proxy_route = PROXY_ROUTE_LOCAL};
    const char *allow = NULL;
    size_t received_bytes = 0, header_end_offset = 0, body_received_bytes = 0;
    bool head_request = false;
    struct timespec header_deadline = *accepted_at;
    header_deadline.tv_sec += HEADER_DEADLINE_SECONDS;
    struct io_context request_io = {running, &header_deadline};
    const struct io_context client_response_io = {running, NULL};
    int status = 0;

    if (set_close_on_exec(client_fd) < 0) {
        perror("client cloexec");
        goto cleanup;
    }

    int result = receive_request_headers(client_fd, request_header, sizeof(request_header),
                                &received_bytes, &header_end_offset, &request_io);
    if (!server_is_running(&request_io)) goto cleanup;
    if (result == 1) {
        if (received_bytes == 0) goto cleanup;
        status = 400;
        goto respond;
    }
    if (result < 0) {
        if (errno == EMSGSIZE) status = 431;
        else if (socket_io_timed_out()) status = 408;
        else { perror("receive request headers"); goto cleanup; }
        goto respond;
    }

    struct timespec body_deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &body_deadline) < 0) {
        status = 500;
        goto respond;
    }
    body_deadline.tv_sec += BODY_DEADLINE_SECONDS;

    status = http_parse_request(request_header, header_end_offset, &request);
    head_request = http_slice_equal(request.method, "HEAD");
    if (status) goto respond;
    status = http_select_route(&request, &selected_route, &allow);
    if (!status && selected_route.proxy_route != PROXY_ROUTE_LOCAL)
        status = proxy_validate_request(&request);
    if (status) goto respond;

    if (request.content_length) {
        request_body = malloc(request.content_length);
        if (!request_body) { status = 500; goto respond; }
        body_received_bytes = received_bytes - header_end_offset;
        if (body_received_bytes > request.content_length) body_received_bytes = request.content_length;
        memcpy(request_body, request_header + header_end_offset, body_received_bytes);
    }

    if (request.expect_continue && body_received_bytes < request.content_length) {
        static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
        if (send_all_with_deadline(client_fd, interim, sizeof(interim) - 1, &client_response_io) < 0) {
            if (server_is_running(&client_response_io)) perror("send 100 Continue");
            goto cleanup;
        }
    }

    request_io.deadline = &body_deadline;
    while (body_received_bytes < request.content_length) {
        ssize_t count = receive_with_deadline(client_fd, request_body + body_received_bytes,
                                     request.content_length - body_received_bytes, &request_io);
        if (!server_is_running(&request_io)) goto cleanup;
        if (count == 0) { status = 400; goto respond; }
        if (count < 0) {
            if (socket_io_timed_out()) { status = 408; goto respond; }
            perror("receive request body");
            goto cleanup;
        }
        body_received_bytes += (size_t)count;
    }

    request.body = (struct http_slice){request_body, body_received_bytes};
    status = 200;

respond:
    if (server_is_running(&client_response_io)) {
        struct router_response response;
        if (status == 200 && selected_route.proxy_route != PROXY_ROUTE_LOCAL) {
            const struct sockaddr_in *backend =
                proxy_select_backend(proxy_config, selection, selected_route.proxy_route);
            proxy_prepare_response(backend, &request, running, &response);
        } else if (status == 200 && selected_route.local_handler)
            selected_route.local_handler(&request, &response);
        else router_prepare_error(&response, status, allow);

        int send_result = send_prepared_response(client_fd, &response, head_request, &client_response_io);
        int send_errno = errno;
        if (send_result == 0) {
            finish_response(client_fd, &client_response_io);
        } else if (server_is_running(&client_response_io)) {
            errno = send_errno;
            perror("send response");
        }
    }
cleanup:
    /* Sole owner of request_body storage; request_io/client_response_io helpers never release it. */
    free(request_body);
}

/* 0: complete, 1: EOF before completion, -1: errno describes the error. */
static int receive_request_headers(int fd, unsigned char *buf, size_t cap,
                          size_t *received, size_t *header_end,
                          const struct io_context *io) {
    *received = 0;
    *header_end = 0;
    if (!buf || cap < 4) { errno = EINVAL; return -1; }

    while (*received < cap) {
        size_t previous = *received;
        ssize_t count = receive_with_deadline(fd, buf + previous, cap - previous, io);
        if (count < 0) return -1;
        if (count == 0) return 1;
        *received += (size_t)count;

        size_t start = previous > 3 ? previous - 3 : 0;
        for (size_t i = start; i + 4 <= *received; ++i) {
            if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                *header_end = i + 4;
                return 0;
            }
        }
    }

    errno = EMSGSIZE;
    return -1;
}

int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len) {
    size_t received, header_end;
    if (receive_request_headers(fd, (unsigned char *)buf, cap, &received, &header_end, NULL) != 0) return -1;
    if (out_len) *out_len = received;
    return 0;
}

/* Stream exactly length bytes from provider; fail with EIO on short/oversized/null chunks. */
static int send_body(int fd, struct body_provider *provider, size_t length,
                     const struct io_context *io) {
    size_t remaining = length;
    while (remaining > 0) {
        struct http_slice chunk = {0};
        errno = 0;
        enum body_result result = provider->next_func(provider->state, &chunk);
        if (result != BODY_DATA) {
            if (result != BODY_ERROR || errno == 0) errno = EIO;
            return -1;
        }
        if (!chunk.data || chunk.length == 0 || chunk.length > remaining) {
            errno = EIO;
            return -1;
        }
        if (send_all_with_deadline(fd, chunk.data, chunk.length, io) < 0) return -1;
        remaining -= chunk.length;
    }
    return 0;
}

/* Stream HTTP/1.1 chunked frames; omit terminating 0-chunk on provider or send error. */
static int send_chunked_body(int fd, struct body_provider *provider,
                             const struct io_context *io) {
    for (;;) {
        struct http_slice chunk = {0};
        errno = 0;
        enum body_result result = provider->next_func(provider->state, &chunk);
        if (result == BODY_END) {
            if (send_all_with_deadline(fd, "0\r\n", 3, io) < 0) return -1;
            if (provider->trailers && provider->trailers->length &&
                send_all_with_deadline(fd, provider->trailers->data,
                                        provider->trailers->length, io) < 0) return -1;
            return send_all_with_deadline(fd, "\r\n", 2, io);
        }
        if (result != BODY_DATA) {
            if (result != BODY_ERROR || errno == 0) errno = EIO;
            return -1;
        }
        if (!chunk.data || chunk.length == 0) {
            errno = EIO;
            return -1;
        }

        char prefix[32];
        int count = snprintf(prefix, sizeof(prefix), "%zx\r\n", chunk.length);
        if (count < 0 || (size_t)count >= sizeof(prefix)) {
            errno = EOVERFLOW;
            return -1;
        }

        if (send_all_with_deadline(fd, prefix, (size_t)count, io) < 0) return -1;
        if (send_all_with_deadline(fd, chunk.data, chunk.length, io) < 0) return -1;
        if (send_all_with_deadline(fd, "\r\n", 2, io) < 0) return -1;
    }
}

/* Format and send HTTP status line, headers, and fixed-length or chunked body. */
static int send_response(int fd, int status, const char *type,
                         struct body_provider *provider, size_t length,
                         bool chunked, const char *allow,
                         bool head_request, const struct io_context *io) {
    char header[512];
    char allow_line[32] = "";
    if (allow) {
        int count = snprintf(allow_line, sizeof(allow_line), "Allow: %s\r\n", allow);
        if (count < 0 || (size_t)count >= sizeof(allow_line)) { errno = EOVERFLOW; return -1; }
    }

    int count;
    if (head_request) {
        /* HEAD is unsupported (501), but must not carry response content. */
        count = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%sConnection: close\r\n\r\n",
                         status, http_reason(status), type, allow_line);
    } else if (chunked) {
        count = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\n"
                         "%sConnection: close\r\n\r\n",
                         status, http_reason(status), type, allow_line);
    } else {
        count = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                         "%sConnection: close\r\n\r\n",
                         status, http_reason(status), type, length, allow_line);
    }
    if (count < 0 || (size_t)count >= sizeof(header)) { errno = EOVERFLOW; return -1; }

    if (send_all_with_deadline(fd, header, (size_t)count, io) < 0) return -1;
    if (head_request) return 0;

    return chunked ? send_chunked_body(fd, provider, io)
                   : send_body(fd, provider, length, io);
}

/* Takes responsibility for provider cleanup, including on send failure. */
static int send_prepared_response(int fd, struct router_response *response,
                                  bool head_request, const struct io_context *io) {
    int result;
    if (response->wire_header.length) {
        result = send_all_with_deadline(fd, response->wire_header.data,
                                        response->wire_header.length, io);
        if (result == 0)
            result = response->chunked
                ? send_chunked_body(fd, &response->body_provider, io)
                : send_body(fd, &response->body_provider, response->length, io);
    } else {
        result = send_response(fd, response->status, response->content_type,
                               &response->body_provider, response->length,
                               response->chunked, response->allow, head_request, io);
    }
    int send_errno = errno;

    if (response->body_provider.cleanup_func)
        response->body_provider.cleanup_func(response->body_provider.state);
    if (result < 0) errno = send_errno;
    return result;
}

/* Half-close write side and drain bounded leftover client input before closing. */
static void finish_response(int fd, const struct io_context *io) {
    if (!server_is_running(io) || shutdown(fd, SHUT_WR) < 0) return;

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) return;
    deadline.tv_sec += DRAIN_DEADLINE_SECONDS;

    struct io_context drain = {io->running, &deadline};
    unsigned char discard[1024];
    size_t discarded = 0;
    while (server_is_running(&drain) && discarded < DRAIN_MAX_BYTES) {
        size_t room = DRAIN_MAX_BYTES - discarded;
        if (room > sizeof(discard)) room = sizeof(discard);
        ssize_t count = receive_with_deadline(fd, discard, room, &drain);
        if (count <= 0) break;
        discarded += (size_t)count;
    }
}
