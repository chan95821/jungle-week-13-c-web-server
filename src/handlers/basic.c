#include "handlers.h"

/* Prepare fixed plain-text greeting for GET / and GET /hello. */
void prepare_hello(const struct http_request *request,
                   struct router_response *response) {
    (void)request;
    router_prepare_borrowed_memory(response, 200, "text/plain", "Hello", 5);
}

/* Echo borrowed request body bytes verbatim for POST /echo. */
void prepare_echo(const struct http_request *request,
                  struct router_response *response) {
    router_prepare_borrowed_memory(response, 200, "application/octet-stream",
                                   request->body.data, request->body.length);
}
