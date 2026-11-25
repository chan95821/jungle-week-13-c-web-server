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
    if (seen_roles != 31U) {
        fprintf(stderr, "%s: submission, query-a, query-b, worker-policy and query-policy are required\n", path);
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

const struct sockaddr_in *proxy_select_backend(
    const struct proxy_config *config, struct proxy_selection_state *selection,
    enum proxy_route route) {
    if (route == PROXY_ROUTE_SUBMISSIONS) return &config->submission_backend;
    if (route != PROXY_ROUTE_QUERY) return NULL;

    struct distribution_candidate candidates[2];
    for(size_t i=0;i<2;++i)candidates[i]=(struct distribution_candidate){0,selection->timing[i],true};
    struct distribution_turn turn=selection->turn,next;
    turn.cursor=(uint32_t)(selection->next_query_backend%2);
    size_t selected_index;
    if(choose_distribution_candidate(config->query_policy,candidates,2,&turn,&selected_index,&next)<0)return NULL;
    selection->turn=next;selection->next_query_backend=next.cursor;
    return &config->query_backends[selected_index];
}

/* Headers are already syntactically validated. Reject protected option tokens
 * before the caller allocates/reads a body, sends 100, or selects a backend. */
int proxy_validate_request(const struct http_request *request) {
    struct http_slice headers = request->headers;
    size_t offset = 0;
    struct http_field field;
    int next;
    while ((next = http_next_field(headers, &offset, &field)) > 0) {
        if (!http_field_name_equal(field.name, "connection")) continue;
        size_t position = 0;
        while (position < field.value.length) {
            size_t start = position;
            while (position < field.value.length && field.value.data[position] != ',')
                ++position;
            size_t end = position;
            while (start < end &&
                   (field.value.data[start] == ' ' || field.value.data[start] == '\t'))
                ++start;
            while (end > start &&
                   (field.value.data[end - 1] == ' ' || field.value.data[end - 1] == '\t'))
                --end;
            struct http_slice option = {field.value.data + start, end - start};
            if (http_field_name_equal(option, "host") ||
                http_field_name_equal(option, "content-length") ||
                http_field_name_equal(option, "transfer-encoding")) return 400;
            if (position < field.value.length) ++position;
        }
    }
    return next < 0 ? 400 : 0;
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

    if(!strcmp(role,"worker-policy") || !strcmp(role,"query-policy")) {
        unsigned int bit=!strcmp(role,"worker-policy")?8U:16U;
        enum distribution_policy policy;
        if((*seen_roles&bit) || parse_distribution_policy(endpoint,&policy)<0)return -1;
        if(bit==8U)config->worker_policy=policy;else config->query_policy=policy;
        *seen_roles|=bit;return 0;
    }
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