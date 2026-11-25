#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"

enum body_result { BODY_DATA, BODY_END, BODY_ERROR };

struct body_provider {
    enum body_result (*next_func)(void *state, struct http_slice *chunk);
    void (*cleanup_func)(void *state);
    void *state;
    const struct http_slice *trailers; /* Optional validated fields after BODY_END. */
};

/* Borrows body storage. A prepared provider may point inside this object:
 * keep it at the same address until provider cleanup completes. */
struct router_response {
    int status;
    const char *content_type;
    const void *body;
    size_t length;
    bool chunked;
    const char *allow;
    struct body_provider body_provider;
    struct http_slice memory_body;
    struct http_slice wire_header; /* Provider owns storage; valid until first next_func or cleanup. */
};

/* Prepare in caller-owned storage so the provider's state remains valid. */
typedef void (*router_handler)(const struct http_request *request,
                               struct router_response *response);

/* Routing owns endpoint roles; proxy selection owns concrete backend addresses. */
enum proxy_route {
    PROXY_ROUTE_LOCAL,
    PROXY_ROUTE_SUBMISSIONS,
    PROXY_ROUTE_QUERY
};

struct request_route {
    router_handler local_handler;
    enum proxy_route proxy_route;
};

/* Match only; no handler execution, proxy validation, I/O or state mutation.
 * Failure resets selected to LOCAL/NULL and sets Allow only for 405. */
int http_select_route(const struct http_request *request,
                      struct request_route *selected, const char **allow);

/* Prepare a text/plain error response using http_reason() and optional Allow header. */
void router_prepare_error(struct router_response *response, int status,
                          const char *allow);

#endif
