#ifndef SERVER_H
#define SERVER_H

#include "http.h"
#include "proxy.h"
#include "socket_io.h"

/* Provisional client policy; backend policy is separately named in proxy.h. */
enum {
    HEADER_DEADLINE_SECONDS = 7,
    BODY_DEADLINE_SECONDS = 15,
    DRAIN_DEADLINE_SECONDS = 1,
    DRAIN_MAX_BYTES = 64 * 1024
};

/* Legacy framing contract: out_len includes any body bytes already received. */
int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len);
/* Borrow client FD, main-owned immutable configuration and mutable selection state.
 * main closes client FD; this function owns request memory until response cleanup. */
void handle_client(int client_fd, const volatile sig_atomic_t *running,
                   const struct timespec *accepted_at,
                   const struct proxy_config *proxy_config,
                   struct proxy_selection_state *selection);

#endif
