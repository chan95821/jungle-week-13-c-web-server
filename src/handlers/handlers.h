#ifndef HANDLERS_H
#define HANDLERS_H

#include "../router.h"

/* Route preparation handlers registered in routes[] (src/router.c). */
void prepare_hello(const struct http_request *request,
                   struct router_response *response);
void prepare_echo(const struct http_request *request,
                  struct router_response *response);
void prepare_cafe(const struct http_request *request,
                  struct router_response *response);
void prepare_menu(const struct http_request *request,
                  struct router_response *response);
void prepare_assets(const struct http_request *request,
                    struct router_response *response);
void prepare_greet(const struct http_request *request,
                   struct router_response *response);
void prepare_cgi(const struct http_request *request,
                 struct router_response *response);

/* Shared helpers defined in src/router.c. */
/* Prepare response from caller-owned or static storage (cleanup_func is NULL). */
void router_prepare_borrowed_memory(struct router_response *response,
                                    int status, const char *content_type,
                                    const void *body, size_t length);
/* Take ownership of heap data; free data on allocation failure and return false. */
bool router_prepare_owned_memory(struct router_response *response,
                                 int status, const char *content_type,
                                 unsigned char *data, size_t length);
/* Map path/open errno values to personal policy status (404 or 500). */
int router_path_error_status(int error);
/* Return true when canonical path lies inside canonical directory on a '/' boundary. */
bool router_inside_directory(const char *directory, const char *path);

#endif
