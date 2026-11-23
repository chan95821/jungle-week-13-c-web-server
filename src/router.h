#ifndef ROUTER_H
#define ROUTER_H

#include <stdint.h>
#include "http.h"

enum http_route { HTTP_ROUTE_HELLO, HTTP_ROUTE_ECHO, HTTP_ROUTE_STATIC, HTTP_ROUTE_GREET };

enum router_body_kind { ROUTER_BODY_MEMORY, ROUTER_BODY_FILE };

/* Borrows body storage or a file FD. Handle_client owns their lifetime. */
struct router_response {
    int status;
    const char *content_type;
    const void *body;
    uintmax_t length;
    enum router_body_kind kind;
    int file_fd;
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