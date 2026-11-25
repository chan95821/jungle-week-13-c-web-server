#include "router.h"
#include "handlers/handlers.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum route_match { ROUTE_EXACT, ROUTE_PREFIX };

/* Yield borrowed memory slice once, then signal BODY_END. */
static enum body_result memory_body_next(void *state, struct http_slice *chunk) {
    struct http_slice *remaining = state;
    if (remaining->length == 0) return BODY_END;
    *chunk = *remaining;
    remaining->length = 0;
    return BODY_DATA;
}

void router_prepare_borrowed_memory(struct router_response *response,
                                    int status, const char *content_type,
                                    const void *body, size_t length) {
    *response = (struct router_response){
        .status = status,
        .content_type = content_type,
        .body = body,
        .length = length
    };
    response->memory_body = (struct http_slice){response->body, response->length};
    response->body_provider = (struct body_provider){
        .next_func = memory_body_next,
        .cleanup_func = NULL,
        .state = &response->memory_body
    };
}

struct owned_body {
    unsigned char *data;
    size_t length;
    size_t sent;
};

/* Yield heap-owned buffer once, then signal BODY_END. */
static enum body_result owned_body_next(void *state, struct http_slice *chunk) {
    struct owned_body *body = state;
    if (body->sent == body->length) return BODY_END;
    *chunk = (struct http_slice){body->data + body->sent, body->length - body->sent};
    body->sent = body->length;
    return BODY_DATA;
}

/* Release heap-owned response buffer and its provider state. */
static void owned_body_cleanup(void *state) {
    struct owned_body *body = state;
    free(body->data);
    free(body);
}

bool router_prepare_owned_memory(struct router_response *response,
                                 int status, const char *content_type,
                                 unsigned char *data, size_t length) {
    struct owned_body *body = malloc(sizeof(*body));
    if (!body) {
        free(data);
        return false;
    }

    *body = (struct owned_body){.data = data, .length = length, .sent = 0};
    *response = (struct router_response){
        .status = status,
        .content_type = content_type,
        .length = length,
        .body_provider = {
            .next_func = owned_body_next,
            .cleanup_func = owned_body_cleanup,
            .state = body
        }
    };

    return true;
}

int router_path_error_status(int error) {
    switch (error) {
        case ENOENT: case ENOTDIR: case EACCES: case EPERM:
        case ELOOP: case ENAMETOOLONG:
            return 404;
        default:
            return 500;
    }
}

bool router_inside_directory(const char *directory, const char *path) {
    size_t length = strlen(directory);
    if (length == 1 && directory[0] == '/') return path[0] == '/';
    return strncmp(directory, path, length) == 0 &&
           (path[length] == '/' || path[length] == '\0');
}

struct route_rule {
    const char *path;
    const char *method;
    enum route_match match;
    router_handler route_handler_func;
    enum proxy_route proxy_route;
};

static const struct route_rule routes[] = {
    {"/", "GET", ROUTE_EXACT, prepare_hello, PROXY_ROUTE_LOCAL},
    {"/hello", "GET", ROUTE_EXACT, prepare_hello, PROXY_ROUTE_LOCAL},
    {"/echo", "POST", ROUTE_EXACT, prepare_echo, PROXY_ROUTE_LOCAL},
    {"/cafe", "GET", ROUTE_EXACT, prepare_cafe, PROXY_ROUTE_LOCAL},
    {"/menu", "GET", ROUTE_EXACT, prepare_menu, PROXY_ROUTE_LOCAL},
    {"/assets/", "GET", ROUTE_PREFIX, prepare_assets, PROXY_ROUTE_LOCAL},
    {"/greet", "GET", ROUTE_EXACT, prepare_greet, PROXY_ROUTE_LOCAL},
    {"/cgi-bin/", "GET", ROUTE_PREFIX, prepare_cgi, PROXY_ROUTE_LOCAL},
    {"/api/submissions", "POST", ROUTE_EXACT, NULL, PROXY_ROUTE_SUBMISSIONS},
    {"/api/submission-status", "GET", ROUTE_EXACT, NULL, PROXY_ROUTE_QUERY},
    {"/api/hello", "GET", ROUTE_EXACT, NULL, PROXY_ROUTE_QUERY},
};

/* Return true when path starts with the literal prefix rule. */
static bool path_has_prefix(struct http_slice path, const char *prefix) {
    size_t length = strlen(prefix);
    return path.length >= length && memcmp(path.data, prefix, length) == 0;
}

/* Match raw request path against either an exact or prefix route rule. */
static bool path_matches_rule(struct http_slice path, const struct route_rule *rule) {
    if (rule->match == ROUTE_PREFIX) return path_has_prefix(path, rule->path);
    return http_slice_equal(path, rule->path);
}

/* Select longest matching route (exact beats prefix on ties, first wins duplicates).
 * Returns 0 on match, or 404 / 405 (with Allow) / 501 on failure. */
static int select_rule(const struct http_request *r,
                       const struct route_rule **selected, const char **allow) {
    *selected = NULL;
    *allow = NULL;

    bool get = http_slice_equal(r->method, "GET");
    bool post = http_slice_equal(r->method, "POST");
    if (!get && !post) return 501;

    const struct route_rule *best = NULL;
    size_t best_length = 0;
    bool allow_get = false, allow_post = false;

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const struct route_rule *rule = &routes[i];
        if (!path_matches_rule(r->path, rule)) continue;

        if (strcmp(rule->method, "GET") == 0) allow_get = true;
        if (strcmp(rule->method, "POST") == 0) allow_post = true;

        size_t length = strlen(rule->path);
        if (http_slice_equal(r->method, rule->method) &&
            (!best || length > best_length ||
             (length == best_length && rule->match == ROUTE_EXACT &&
              best->match == ROUTE_PREFIX))) {
            best = rule;
            best_length = length;
        }
    }

    if (best) {
        *selected = best;
        return 0;
    }
    if (!allow_get && !allow_post) return 404;

    *allow = allow_get && allow_post ? "GET, POST" : allow_get ? "GET" : "POST";
    return 405;
}

int http_select_route(const struct http_request *request,
                      struct request_route *selected, const char **allow) {
    *selected = (struct request_route){.proxy_route = PROXY_ROUTE_LOCAL};
    const struct route_rule *rule;
    int status = select_rule(request, &rule, allow);
    if (status) return status;
    *selected = (struct request_route){
        .local_handler = rule->route_handler_func,
        .proxy_route = rule->proxy_route
    };
    return 0;
}

void router_prepare_error(struct router_response *response, int status,
                          const char *allow) {
    const char *reason = http_reason(status);
    router_prepare_borrowed_memory(response, status, "text/plain",
                                   reason, strlen(reason));
    response->allow = allow;
}
