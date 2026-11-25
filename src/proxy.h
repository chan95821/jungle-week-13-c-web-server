#ifndef PROXY_H
#define PROXY_H

#include "http.h"
#include "distribution_policy.h"
#include "router.h"
#include <signal.h>
#include <netinet/in.h>

/* Caller-owned immutable configuration; no allocations or open FDs inside. */
struct proxy_config {
    struct sockaddr_in submission_backend;
    struct sockaddr_in query_backends[2];
    enum distribution_policy worker_policy, query_policy;
};

/* Caller-owned state. Both query routes share one sequence, initially zero. */
struct proxy_selection_state {
    size_t next_query_backend;
    struct distribution_turn turn;
    struct distribution_timing timing[2];
};

/* Read all three roles and both explicit policies from a file. On failure, preserve config and set errno. */
int proxy_load_config(const char *path, struct proxy_config *config);

/* Call only after successfully selecting a proxy route, before body/100/backend I/O.
 * Return 400 for protected Connection nominations, otherwise 0. No I/O/state changes. */
int proxy_validate_request(const struct http_request *request);

/* Borrow an endpoint from config; NULL for LOCAL/invalid route.
 * Advance QUERY's turn immediately, independently of subsequent connection success.
 * This sequential state is not safe for concurrent worker access without a policy. */
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
/* Sequential compatibility path: optional borrowed query timing state.
 * Concurrent worker path uses shared reservation APIs instead. */
void proxy_prepare_tracked_response(const struct sockaddr_in *backend,
    const struct http_request *request,const volatile sig_atomic_t *running,
    struct router_response *response,struct distribution_timing *timing);
void proxy_prepare_response(const struct sockaddr_in *backend,
                            const struct http_request *request,
                            const volatile sig_atomic_t *running,
                            struct router_response *response);

#endif