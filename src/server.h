#ifndef SERVER_H
#define SERVER_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "http.h"

/* Provisional policy constants; each can be reconsidered independently. */
enum {
    LISTEN_BACKLOG = 128,
    IO_CALL_TIMEOUT_SECONDS = 5,
    HEADER_DEADLINE_SECONDS = 7,
    BODY_DEADLINE_SECONDS = 15,
    DRAIN_DEADLINE_SECONDS = 1,
    DRAIN_MAX_BYTES = 64 * 1024
};

int parse_port(const char *arg, uint16_t *out_port);
int open_listenfd(uint16_t port);
/* Legacy framing contract: out_len includes any body bytes already received. */
int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len);
int send_all(int fd, const void *buf, size_t len);
/* Borrow the FD and cancellation flag. Main closes the FD exactly once. */
void handle_client(int connfd, const volatile sig_atomic_t *running,
                   const struct timespec *accepted_at);

#endif
