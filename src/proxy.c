#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "proxy/internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static const struct sockaddr_in *select_backend(enum proxy_route route,
                                                 struct proxy_config *config);
static int backend_error_status(void);
static int send_backend_request(int backend_fd, const unsigned char *header,
                                  size_t header_length, struct http_slice body,
                                  const volatile sig_atomic_t *running);
static int parse_backend_port(const char *text, unsigned int *port);

/* 중계 순서: 요청 준비 → 목적지 선택 → 연결·송신 → 응답 준비 → 본문 중계 → 정리.
 * 세부 구현은 내부 모듈에 두고, FD 소유권과 응답 시작 경계는 여기서 확인한다. */
int proxy_forward(int client, const struct http_request *request,
                  const unsigned char *header, size_t header_length,
                  enum proxy_route route, struct proxy_config *config,
                  const volatile sig_atomic_t *running) {
    unsigned char wire[PROXY_WIRE_CAPACITY];
    size_t wire_length;
    if (!proxy_build_request_headers(header, header_length, request, wire, &wire_length)) return 400;

    const struct sockaddr_in *address = select_backend(route, config);
    int backend_fd = proxy_connect_backend(address, running);
    if (backend_fd < 0) return backend_error_status();

    /* backend_fd는 연결 성공부터 이 함수가 소유한다. 아래의 모든 경로는
     * cleanup을 거친다. client FD는 빌린 것이므로 여기서 닫지 않는다. */
    int result;
    int saved_client_flags = -1;
    if (send_backend_request(backend_fd, wire, wire_length, request->body, running) < 0) {
        result = backend_error_status();
        goto cleanup;
    }

    struct proxy_reader reader = {.backend_fd = backend_fd, .running = running};
    struct proxy_response response;
    if (proxy_prepare_response_headers(&reader, &response, wire, &wire_length) < 0) {
        result = backend_error_status();
        goto cleanup;
    }

    saved_client_flags = fcntl(client, F_GETFL);
    if (saved_client_flags < 0 ||
        fcntl(client, F_SETFL, saved_client_flags | O_NONBLOCK) < 0) {
        result = 500;
        goto cleanup;
    }

    /* 첫 send가 부분 성공했을 수 있다. 이 경계 뒤에는 -1 또는 완료 0만
     * 반환해 server가 기존 응답 뒤에 새로운 502/504를 붙이지 못하게 한다. */
    result = -1;
    if (proxy_write_all(client, wire, wire_length, PROXY_CLIENT_TIMEOUT_MS, running) < 0) goto cleanup;
    result = proxy_relay_response_body(&reader, client, &response);

cleanup: {
        int saved_errno = errno;
        close(backend_fd);
        if (saved_client_flags >= 0 && fcntl(client, F_SETFL, saved_client_flags) < 0) return -1;
        errno = saved_errno;
        return proxy_is_running(running) ? result : -1;
    }
}

int proxy_parse_endpoint(const char *text, struct sockaddr_in *address) {
    const char *colon = strchr(text, ':');
    char ip[INET_ADDRSTRLEN];
    if (!colon || colon == text || (size_t)(colon - text) >= sizeof(ip)) return -1;

    size_t ip_length = (size_t)(colon - text);
    memcpy(ip, text, ip_length);
    ip[ip_length] = '\0';

    unsigned int port;
    if (parse_backend_port(colon + 1, &port) < 0) return -1;
    *address = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port)
    };
    return port != 0 && inet_pton(AF_INET, ip, &address->sin_addr) == 1 ? 0 : -1;
}

int proxy_select_route(const struct http_request *request, const char **allow) {
    const char *expected_method;
    enum proxy_route route;
    if (http_slice_equal(request->path, "/api/submissions")) {
        expected_method = "POST";
        route = PROXY_SUBMISSION;
    } else if (http_slice_equal(request->path, "/api/submission-status")) {
        expected_method = "GET";
        route = PROXY_STATUS;
    } else {
        return PROXY_LOCAL;
    }

    if (http_slice_equal(request->method, expected_method)) return route;
    bool supported_method = http_slice_equal(request->method, "GET") ||
                            http_slice_equal(request->method, "POST");
    if (!supported_method) return -501;

    *allow = expected_method;
    return -405;
}

/* 조회 순번은 원래와 같이 연결 시도 전에 이동한다. 실패해도 다른 목적지로
 * 재시도하지 않는다. 설정과 순번은 main에서 빌리며 연결 상태는 저장하지 않는다. */
static const struct sockaddr_in *select_backend(enum proxy_route route,
                                                 struct proxy_config *config) {
    if (route != PROXY_STATUS) return &config->submission;

    const struct sockaddr_in *selected = &config->status[config->next_status];
    config->next_status = (config->next_status + 1) % 2;
    return selected;
}

static int send_backend_request(int backend_fd, const unsigned char *header,
                                  size_t header_length, struct http_slice body,
                                  const volatile sig_atomic_t *running) {
    if (proxy_write_all(backend_fd, header, header_length, PROXY_BACKEND_TIMEOUT_MS, running) < 0) return -1;
    return proxy_write_all(backend_fd, body.data, body.length, PROXY_BACKEND_TIMEOUT_MS, running);
}

static int backend_error_status(void) {
    if (errno == ECANCELED) return -1;
    if (errno == ETIMEDOUT) return 504;
    if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) return 500;
    return 502;
}

static int parse_backend_port(const char *text, unsigned int *port) {
    *port = 0;
    if (*text == '\0') return -1;

    for (const char *digit = text; *digit; ++digit) {
        if (*digit < '0' || *digit > '9' ||
            *port > (65535U - (unsigned int)(*digit - '0')) / 10U) return -1;
        *port = *port * 10U + (unsigned int)(*digit - '0');
    }
    return 0;
}
