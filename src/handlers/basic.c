#include "handlers.h"

/* Prepare fixed plain-text greeting for GET / and GET /hello. */
void prepare_hello(const struct http_request *request,
                   const struct cancel_signal *cancel,
                   struct router_response *response) {
    (void)request;
    (void)cancel;
    router_prepare_borrowed_memory(response, 200, "text/plain", "Hello", 5);
}

/* Echo borrowed request body bytes verbatim for POST /echo. */
void prepare_echo(const struct http_request *request,
                  const struct cancel_signal *cancel,
                  struct router_response *response) {
    (void)cancel;
    router_prepare_borrowed_memory(response, 200, "application/octet-stream",
                                   request->body.data, request->body.length);
}
