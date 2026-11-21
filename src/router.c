#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <string.h>

typedef struct {
    HttpStatus status_code;
    const char *reason;
    const char *body;
} HttpStatusEntry;

typedef void (*RouteHandler)(const HttpRequest *req, HttpResponse *resp);

typedef struct {
    const char *method;
    const char *path;
    RouteHandler handler;
} RouteEntry;

static const HttpStatusEntry ERROR_STATUS_TABLE[] = {
    { HTTP_STATUS_BAD_REQUEST,                "Bad Request",                "400 Bad Request\n" },
    { HTTP_STATUS_NOT_FOUND,                  "Not Found",                  "404 Not Found\n" },
    { HTTP_STATUS_METHOD_NOT_ALLOWED,         "Method Not Allowed",         "405 Method Not Allowed\n" },
    { HTTP_STATUS_LENGTH_REQUIRED,            "Length Required",            "411 Length Required\n" },
    { HTTP_STATUS_PAYLOAD_TOO_LARGE,          "Payload Too Large",          "413 Payload Too Large\n" },
    { HTTP_STATUS_URI_TOO_LONG,               "URI Too Long",               "414 URI Too Long\n" },
    { HTTP_STATUS_NOT_IMPLEMENTED,            "Not Implemented",            "501 Not Implemented\n" },
    { HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED, "HTTP Version Not Supported", "505 HTTP Version Not Supported\n" },
};

void set_error_response(HttpResponse *resp, HttpStatus status) {
    if (resp == NULL) {
        return;
    }

    for (size_t i = 0; i < sizeof(ERROR_STATUS_TABLE) / sizeof(ERROR_STATUS_TABLE[0]); i++) {
        if (ERROR_STATUS_TABLE[i].status_code == status) {
            resp->status_code = ERROR_STATUS_TABLE[i].status_code;
            resp->reason = ERROR_STATUS_TABLE[i].reason;
            resp->content_type = "text/plain";
            resp->body = (const uint8_t *)ERROR_STATUS_TABLE[i].body;
            resp->body_len = strlen(ERROR_STATUS_TABLE[i].body);
            resp->owns_body = 0;
            return;
        }
    }

    resp->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    resp->reason = "Internal Server Error";
    resp->content_type = "text/plain";
    resp->body = (const uint8_t *)"500 Internal Server Error\n";
    resp->body_len = strlen("500 Internal Server Error\n");
    resp->owns_body = 0;
}

/* 정적 리터럴 "Hello"를 비소유(owns_body=0) 200 OK 응답으로 설정한다. */
static void handle_get_hello(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    resp->status_code = HTTP_STATUS_OK;
    resp->reason = "OK";
    resp->content_type = "text/plain";
    resp->body = (const uint8_t *)"Hello";
    resp->body_len = 5;
    resp->owns_body = 0;
}

/* req->body를 복사 없이 참조(owns_body=0)하는 200 OK 에코 응답을 설정한다. */
static void handle_post_echo(const HttpRequest *req, HttpResponse *resp) {
    resp->status_code = HTTP_STATUS_OK;
    resp->reason = "OK";
    resp->content_type = req->has_content_type ? req->content_type : "application/octet-stream";
    resp->body = req->body;
    resp->body_len = req->body_len;
    resp->owns_body = 0;
}

static const RouteEntry ROUTES[] = {
    { "GET",  "/",      handle_get_hello },
    { "GET",  "/hello", handle_get_hello },
    { "POST", "/echo",  handle_post_echo },
};

void dispatch_request(const HttpRequest *req, HttpResponse *resp) {
    if (req == NULL || resp == NULL) {
        return;
    }

    if (strcmp(req->method, "POST") == 0 && !req->has_content_length) {
        set_error_response(resp, HTTP_STATUS_LENGTH_REQUIRED);
        return;
    }

    const char *q = strchr(req->uri, '?');
    size_t path_len = (q != NULL) ? (size_t)(q - req->uri) : strlen(req->uri);

    int path_matched = 0;
    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        size_t route_len = strlen(ROUTES[i].path);
        if (path_len == route_len && memcmp(req->uri, ROUTES[i].path, path_len) == 0) {
            path_matched = 1;
            if (strcmp(req->method, ROUTES[i].method) == 0) {
                ROUTES[i].handler(req, resp);
                return;
            }
        }
    }

    set_error_response(resp, path_matched ? HTTP_STATUS_METHOD_NOT_ALLOWED : HTTP_STATUS_NOT_FOUND);
}
