#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"

enum body_result { BODY_DATA, BODY_END, BODY_ERROR };

struct body_provider {
    enum body_result (*next_func)(void *state, struct http_slice *chunk);
    void (*cleanup_func)(void *state);
    void *state;
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
};

/* Prepare in caller-owned storage so the provider's state remains valid. */
typedef void (*router_handler)(const struct http_request *request,
                               struct router_response *response);

/* Select a route_handler_func before receiving the body; invoke it after body reception. */
int http_select_handler(const struct http_request *request,
                        router_handler *route_handler_func, const char **allow);

/* Prepare a text/plain error response using http_reason() and optional Allow header. */
void router_prepare_error(struct router_response *response, int status,
                          const char *allow);

#endif
