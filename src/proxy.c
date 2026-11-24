#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "socket_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_config_line(char *line, struct proxy_config *config,
                             unsigned int *seen_roles);
static int parse_backend_address(char *endpoint, struct sockaddr_in *address);

/* Top-down entry: open -> read/validate lines -> close -> publish config. */
int proxy_load_config(const char *path, struct proxy_config *config) {
    if (!path || !config) { errno = EINVAL; return -1; }
    FILE *file = fopen(path, "r");
    if (!file) return -1;

    struct proxy_config parsed_config = {0};
    unsigned int seen_roles = 0;
    char *line = NULL;
    size_t line_capacity = 0;
    size_t line_number = 0;
    int result = -1;
    ssize_t line_bytes;
    errno = 0;
    while ((line_bytes = getline(&line, &line_capacity, file)) >= 0) {
        ++line_number;
        if (memchr(line, '\0', (size_t)line_bytes) ||
            parse_config_line(line, &parsed_config, &seen_roles) < 0) {
            fprintf(stderr, "%s:%zu: invalid or duplicate proxy setting\n",
                    path, line_number);
            errno = EINVAL;
            goto cleanup;
        }
    }
    if (!feof(file)) {
        if (errno == 0) errno = EIO;
        goto cleanup;
    }
    if (seen_roles != 7U) {
        fprintf(stderr, "%s: submission, query-a and query-b are required\n", path);
        errno = EINVAL;
        goto cleanup;
    }
    result = 0;

cleanup:
    {
        int saved_errno = errno;
        free(line);
        if (fclose(file) != 0) {
            if (result == 0) return -1;
        }
        if (result < 0) { errno = saved_errno; return -1; }
    }
    *config = parsed_config;
    return 0;
}

int proxy_match_request(const struct http_request *request,
                        enum proxy_route *route, const char **allow) {
    *route = PROXY_ROUTE_LOCAL;
    *allow = NULL;

    enum proxy_route matched_route;
    const char *required_method;
    if (http_slice_equal(request->path, "/api/submissions")) {
        matched_route = PROXY_ROUTE_SUBMISSIONS;
        required_method = "POST";
    } else if (http_slice_equal(request->path, "/api/submission-status") ||
               http_slice_equal(request->path, "/api/hello")) {
        matched_route = PROXY_ROUTE_QUERY;
        required_method = "GET";
    } else {
        return 0;
    }

    if (!http_slice_equal(request->method, "GET") &&
        !http_slice_equal(request->method, "POST")) return 501;
    if (!http_slice_equal(request->method, required_method)) {
        *allow = required_method;
        return 405;
    }
    *route = matched_route;
    return 0;
}

const struct sockaddr_in *proxy_select_backend(
    const struct proxy_config *config, struct proxy_selection_state *selection,
    enum proxy_route route) {
    if (route == PROXY_ROUTE_SUBMISSIONS) return &config->submission_backend;
    if (route != PROXY_ROUTE_QUERY) return NULL;

    size_t selected_index = selection->next_query_backend % 2;
    selection->next_query_backend = (selected_index + 1) % 2;
    return &config->query_backends[selected_index];
}

/* Parse one role/address pair. Blank lines and # comments have no effect. */
static int parse_config_line(char *line, struct proxy_config *config,
                             unsigned int *seen_roles) {
    char *comment = strchr(line, '#');
    if (comment) *comment = '\0';

    char *cursor = NULL;
    const char *role = strtok_r(line, " \t\r\n", &cursor);
    if (!role) return 0;
    char *endpoint = strtok_r(NULL, " \t\r\n", &cursor);
    if (!endpoint || strtok_r(NULL, " \t\r\n", &cursor)) return -1;

    struct sockaddr_in *address;
    unsigned int role_bit;
    if (strcmp(role, "submission") == 0) {
        address = &config->submission_backend;
        role_bit = 1U;
    } else if (strcmp(role, "query-a") == 0) {
        address = &config->query_backends[0];
        role_bit = 2U;
    } else if (strcmp(role, "query-b") == 0) {
        address = &config->query_backends[1];
        role_bit = 4U;
    } else {
        return -1;
    }
    if ((*seen_roles & role_bit) || parse_backend_address(endpoint, address) < 0)
        return -1;
    *seen_roles |= role_bit;
    return 0;
}

/* Numeric IPv4 only; reuse the existing decimal port validator. */
static int parse_backend_address(char *endpoint, struct sockaddr_in *address) {
    char *colon = strchr(endpoint, ':');
    if (!colon || strchr(colon + 1, ':')) return -1;
    *colon = '\0';
    uint16_t port;
    if (parse_port(colon + 1, &port) < 0) return -1;

    struct sockaddr_in parsed_address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, endpoint, &parsed_address.sin_addr) != 1) return -1;
    *address = parsed_address;
    return 0;
}