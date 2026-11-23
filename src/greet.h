#ifndef GREET_H
#define GREET_H

#include "router.h"

/* Caller owns *owned_body; helpers and senders only borrow the returned bytes. */
struct router_response greet_prepare_response(const struct http_request *request,
                                               unsigned char **owned_body);

#endif