#include "handlers.h"

void prepare_hello(const struct http_request *request,
                   struct router_response *response) {
    (void)request;
    router_prepare_borrowed_memory(response, 200, "text/plain", "Hello", 5);
}

void prepare_echo(const struct http_request *request,
                  struct router_response *response) {
    router_prepare_borrowed_memory(response, 200, "application/octet-stream",
                                   request->body.data, request->body.length);
}
