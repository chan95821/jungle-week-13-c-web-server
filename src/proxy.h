#ifndef PROXY_H
#define PROXY_H

#include "http.h"
#include "router.h"
#include "socket_io.h"
#include <pthread.h>
#include <netinet/in.h>

/* Caller-owned immutable configuration; no allocations or open FDs inside. */
struct proxy_config {
    struct sockaddr_in submission_backend;
    struct sockaddr_in query_backends[2];
};

/* Caller-owned state initialized via proxy_selection_init().
 * Both query routes share one mutex-protected round-robin sequence. */
struct proxy_selection_state {
    pthread_mutex_t lock;
    size_t next_query_backend;
};

/* Initialize query selection state and its mutex. Return 0, or -1 with errno. */
int proxy_selection_init(struct proxy_selection_state *selection);

/* Destroy query selection state's mutex. Safe no-op when selection is NULL. */
void proxy_selection_destroy(struct proxy_selection_state *selection);

/* Read all three roles from a file. On failure, preserve config and set errno. */
int proxy_load_config(const char *path, struct proxy_config *config);

/* Call only after successfully selecting a proxy route, before body/100/backend I/O.
 * Return 400 for protected Connection nominations, otherwise 0. No I/O/state changes. */
int proxy_validate_request(const struct http_request *request);

/* Borrow an endpoint from config; NULL for LOCAL/invalid route.
 * selection must be initialized with proxy_selection_init().
 * For PROXY_ROUTE_QUERY, synchronize round-robin selection using selection->lock
 * and advance the turn immediately, independently of subsequent connection success. */
const struct sockaddr_in *proxy_select_backend(
    const struct proxy_config *config, struct proxy_selection_state *selection,
    enum proxy_route route);

enum {
    PROXY_CONNECT_SECONDS = 1,
    PROXY_REQUEST_SECONDS = 5,
    PROXY_HEADER_SECONDS = 1,
    PROXY_BODY_IDLE_SECONDS = 5,
    PROXY_BODY_SECONDS = 15,
    PROXY_STREAM_BYTES = 16 * 1024
};
/* Prepare backend stream or a local 500/502/504 response. Takes no request ownership.
 * On success response's provider owns backend FD and all retained proxy buffers.
 * wire_header must be sent before the first next_func call, which releases it. */
void proxy_prepare_response(const struct sockaddr_in *backend,
                            const struct http_request *request,
                            const struct cancel_signal *cancel,
                            struct router_response *response);

#endif