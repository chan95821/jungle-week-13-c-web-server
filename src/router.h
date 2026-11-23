#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"

enum http_route { HTTP_ROUTE_HELLO, HTTP_ROUTE_ECHO, HTTP_ROUTE_STATIC };

/* Borrows body storage owned by handle_client or a static literal. */
struct router_response {
    int status;
    const char *content_type;
    const void *body;
    size_t length;
    const char *allow;
};

/* Decide the route before receiving a body; return 0 or an HTTP error status. */
int http_route_request(const struct http_request *request,
                       enum http_route *route, const char **allow);

/* Select content only. Does not allocate, free, or perform socket I/O. */
struct router_response router_prepare_response(const struct http_request *request,
                                               enum http_route route, int status,
                                               const char *allow);

#endif
