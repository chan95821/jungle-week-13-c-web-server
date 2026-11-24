#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "io_utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Development output bound, not an HTTP protocol limit. */
#define PROXY_REQUEST_HEADER_MAX_BYTES (16 * 1024)
/* 개인 검증 기본값: 헤더와 본문이 함께 사용하는 총 송신 제한이다. */
#define BACKEND_SEND_TIMEOUT_MS 1000
/* 최종 응답 헤더를 기다리는 총 제한. Issue #17의 백엔드 무응답 3초 조건을 따른다.
 * 연결·요청 송신·본문 무진행 제한(1초)과 구분하며, 부분 헤더 수신으로 갱신하지 않는다. */
#define BACKEND_RESPONSE_HEADER_TIMEOUT_MS 3000

static int acquire_proxy_backend(const struct proxy_upstream *upstream, int *backend_fd);
static int validate_upstream_config(const struct proxy_upstream *upstream);
static int send_proxy_request(int backend_fd, struct http_slice headers,
                               struct http_slice body);
static int receive_proxy_response(int backend_fd, const struct http_request *request,
                                    struct router_response *response);
static int prepare_backend_response(int backend_fd, const struct http_request *request,
                                      struct http_slice headers, struct http_slice prefetched_body,
                                      struct router_response *response);
static int prepare_backend_body(int backend_fd, const struct http_response_headers *parsed,
                                  struct http_slice prefetched_body, struct router_response *response);

struct header_output {
    char *data;
    size_t capacity;
    size_t used;
};

/* 아래에서 정의하는 헤더 선별·기록 보조 함수. prepare_backend_body()가 먼저 사용한다. */
static bool should_skip_backend_response_header(struct http_slice headers, struct http_slice name);
static bool append_header_bytes(struct header_output *output, struct http_slice value);
static struct http_slice http_slice_from_string(const char *text);

/* 라우트가 넘긴 업스트림 설정으로 프록시 응답을 준비한다(경로로 목적지를 고르지 않음).
 * 헤더 직렬화(400/500) → 백엔드 연결 확보(502/504 가능) → 요청 송신 → 응답 헤더 수신·검증·본문 공급자 준비
 * 순으로 조율한다. 어느 단계든 실패하면 failure:에서 열린 backend_fd를 닫고 오류 응답을 준비한다.
 * 성공하면 FD 소유권이 응답의 body_provider로 넘어가므로 여기서 닫지 않는다.
 * 남은 범위: 중간 1xx 수신 루프, HEAD·chunked 응답 중계, 일반 응답 헤더 보존. */
void prepare_proxy_response(const struct http_request *request,
                            const struct proxy_upstream *upstream,
                            struct router_response *response) {
    char headers[PROXY_REQUEST_HEADER_MAX_BYTES];
    size_t header_length = 0;
    int backend_fd = -1;
    int status = build_proxy_request_headers(request, headers, sizeof(headers), &header_length);
    if (status) goto failure;

    status = acquire_proxy_backend(upstream, &backend_fd);
    if (status) goto failure;

    status = send_proxy_request(backend_fd,
                                (struct http_slice){(const unsigned char *)headers, header_length},
                                request->body);
    if (status) goto failure;

    status = receive_proxy_response(backend_fd, request, response);
    if (status) goto failure;

    /* On success, receive_proxy_response has closed or transferred the backend FD. */
    return;

failure:
    if (backend_fd >= 0) close(backend_fd);
    router_prepare_error(response, status, NULL);
}

/* 선택 정책이 고른 후보에 연결하고 결과를 정책에 통보한다. 제외·복귀 상태는 직접 다루지 않는다.
 * 현재 요청의 중복 시도만 막으며, 연결 실패(502/504)는 허용 횟수 내에서 다른 후보로 우회한다.
 * 성공 시 0과 호출자 소유 FD를 반환한다. 설정/자원 오류는 500, 후보 소진은 502다.
 * 실제 한 서버 연결과 연결 제한 시간은 connect_backend()에 위임한다. */
static int acquire_proxy_backend(const struct proxy_upstream *upstream, int *backend_fd) {
    *backend_fd = -1;
    int status = validate_upstream_config(upstream);
    if (status) return status;

    if (!upstream->select_backend) {
        return connect_backend(&upstream->backends[0], backend_fd);
    }

    bool *available_for_this_request = calloc(upstream->backend_count,
                                              sizeof(*available_for_this_request));
    if (!available_for_this_request) return 500;

    for (size_t i = 0; i < upstream->backend_count; ++i) {
        available_for_this_request[i] = true;
    }

    status = 502;
    for (size_t attempt = 0; attempt < upstream->max_attempts; ++attempt) {
        size_t selected;
        status = upstream->select_backend(upstream->group_state, available_for_this_request,
                                           upstream->backend_count, &selected);
        if (status) break;
        if (selected >= upstream->backend_count || !available_for_this_request[selected]) {
            status = 500;
            break;
        }

        /* 같은 요청에서 이미 시도한 서버는 선택 정책이 달라도 다시 시도하지 않는다. */
        available_for_this_request[selected] = false;

        status = connect_backend(&upstream->backends[selected], backend_fd);
        if (upstream->on_connection_result) {
            int record_status = upstream->on_connection_result(upstream->group_state, selected, status);
            if (record_status) {
                if (*backend_fd >= 0) close(*backend_fd);
                *backend_fd = -1;
                status = record_status;
                break;
            }
        }
        if (status == 0) break;

        /* 내부 오류(500)는 백엔드 장애가 아니므로 우회하지 않는다. */
        if (status != 502 && status != 504) break;

        status = 502;
    }

    free(available_for_this_request);
    return status;
}

/* 연결 전 설정 계약을 확인한다. 고정 주소는 한 곳·1회, 그룹은 상태와 최대 2회가 필요하다.
 * 네트워크나 그룹 상태를 변경하지 않으며 유효하면 0, 잘못된 설정이면 500을 반환한다. */
static int validate_upstream_config(const struct proxy_upstream *upstream) {
    if (!upstream || !upstream->backends || upstream->backend_count == 0 ||
        upstream->max_attempts == 0) return 500;

    if (!upstream->select_backend) {
        return upstream->backend_count == 1 && upstream->max_attempts == 1 ? 0 : 500;
    }

    if (!upstream->group_state || upstream->max_attempts > 2 ||
        upstream->max_attempts > upstream->backend_count) return 500;
    return 0;
}

/* 헤더와 이미 수신한 본문을 같은 절대 마감 내에서 차례대로 완전 송신한다.
 * FD·버퍼는 빌리며 실패해도 닫지 않는다. 상위 prepare_proxy_response()가 정리하고 재시도하지 않는다.
 * 성공 0, 시간 초과 504, 연결 종료/장애 502, 잘못된 버퍼나 로컬 오류 500을 반환한다. */
static int send_proxy_request(int backend_fd, struct http_slice headers,
                                struct http_slice body) {
    struct timespec deadline;
    if (!make_io_deadline(&deadline, BACKEND_SEND_TIMEOUT_MS)) return 500;
    const struct io_context output = {.deadline = &deadline};

    if (send_all_bytes(backend_fd, headers.data, headers.length, &output) < 0 ||
        send_all_bytes(backend_fd, body.data, body.length, &output) < 0) {
        if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) return 504;
        if (errno == EPIPE || errno == ECONNRESET || errno == ECONNABORTED ||
            errno == ENOTCONN || errno == EIO) return 502;
        return 500;
    }
    return 0;
}

/* 첫 응답 헤더 블록을 읽어 헤더와 동반 본문을 분리하고 해석·검증 단계로 넘긴다.
 * 중간 1xx를 건너뛰고 최종 응답까지 읽는 루프는 아직 없다(1xx는 준비 단계가 502로 거부).
 * 성공하면 본문 공급자가 FD 소유권을 인수하고, 실패하면 호출자가 닫는다.
 * 조기 EOF/상한 초과는 502, 무응답 시간 초과는 504다. */
static int receive_proxy_response(int backend_fd, const struct http_request *request,
                                    struct router_response *response) {
    unsigned char buffer[REQ_HEADER_MAX_BYTES];
    size_t received, header_end;
    struct timespec deadline;
    if (!make_io_deadline(&deadline, BACKEND_RESPONSE_HEADER_TIMEOUT_MS)) return 500;
    const struct io_context input = {.deadline = &deadline};
    int result = receive_http_headers(backend_fd, buffer, sizeof(buffer), &received, &header_end, &input);
    if (result == 1) return 502;
    if (result < 0) {
        if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) return 504;
        if (errno == EMSGSIZE || errno == ECONNRESET || errno == ENOTCONN) return 502;
        return 500;
    }

    return prepare_backend_response(backend_fd, request,
                                     (struct http_slice){buffer, header_end},
                                     (struct http_slice){buffer + header_end, received - header_end},
                                     response);
}

/* 수신한 헤더를 해석·검증한 뒤 본문 공급자 준비 단계로 넘긴다.
 * 해석 결과는 입력 버퍼를 빌리므로 밖으로 보관하지 않고, 필요한 값은 다음 단계가 복사한다.
 * 성공 전까지 FD는 호출자에게 남으며, 실패하면 prepare_proxy_response()가 닫는다. */
static int prepare_backend_response(int backend_fd, const struct http_request *request,
                                      struct http_slice headers, struct http_slice prefetched_body,
                                      struct router_response *response) {
    (void)request;
    struct http_response_headers parsed;
    int status = http_parse_response_headers(headers.data, headers.length, &parsed);
    if (status) return status;
    status = validate_backend_response_headers(&parsed);
    if (status) return status;

    return prepare_backend_body(backend_fd, &parsed, prefetched_body, response);
}

/* 본문을 한 번에 읽는 최대 크기와, 백엔드 본문 수신이 멈췄다고 판단하는 무진행 제한이다.
 * 무진행 제한은 클라이언트가 느린 경우가 아니라 백엔드가 새 바이트를 주지 않는 경우에만 적용한다. */
#define PROXY_BODY_CHUNK_MAX_BYTES (16 * 1024)
#define BACKEND_BODY_IDLE_TIMEOUT_MS 1000

/* Content-Type 문자열과 동반 본문을 한 할당으로 소유하는 공급자 상태다.
 * storage 뒤쪽에 NUL 종단 Content-Type을 두고 그다음에 동반 본문 바이트가 이어진다. */
struct proxy_backend_body {
    int backend_fd;
    size_t unread_length;
    size_t prefetched_length;
    size_t prefetched_sent;
    const char *content_type;
    unsigned char *prefetched;
    unsigned char buffer[PROXY_BODY_CHUNK_MAX_BYTES];
    char storage[];
};

/* 먼저 동반 본문을 한 번 제공하고, 이후에는 선언한 남은 길이만큼만 백엔드에서 읽는다.
 * 선언 길이 전에 EOF·오류·무진행이 오면 BODY_ERROR로 불완전 응답을 드러낸다.
 * FD와 상태는 cleanup_func가 정리하므로 이 함수는 닫거나 해제하지 않는다. */
static enum body_result proxy_backend_body_next(void *opaque, struct http_slice *chunk) {
    struct proxy_backend_body *body = opaque;
    if (body->prefetched_sent < body->prefetched_length) {
        *chunk = (struct http_slice){body->prefetched + body->prefetched_sent,
                                     body->prefetched_length - body->prefetched_sent};
        body->prefetched_sent = body->prefetched_length;
        return BODY_DATA;
    }
    if (body->unread_length == 0) return BODY_END;

    size_t wanted = body->unread_length < sizeof(body->buffer)
                    ? body->unread_length : sizeof(body->buffer);
    struct timespec deadline;
    if (!make_io_deadline(&deadline, BACKEND_BODY_IDLE_TIMEOUT_MS)) return BODY_ERROR;
    const struct io_context input = {.deadline = &deadline};
    ssize_t count = receive_socket_bytes(body->backend_fd, body->buffer, wanted, &input);
    if (count <= 0) return BODY_ERROR;

    body->unread_length -= (size_t)count;
    *chunk = (struct http_slice){body->buffer, (size_t)count};
    return BODY_DATA;
}

/* 송신 성공·실패 모두에서 호출된다. 남은 백엔드 FD를 닫고 응답 소유 메모리를 해제한다. */
static void proxy_backend_body_cleanup(void *opaque) {
    struct proxy_backend_body *body = opaque;
    if (body->backend_fd >= 0) close(body->backend_fd);
    free(body);
}

/* 해석·검증이 끝난 최종 응답을 기존 router_response와 body_provider로 옮긴다.
 * 1xx 거부는 validate_backend_response_headers()가 이미 결정했으므로 여기서는 다시 판단하지 않는다.
 * 성공하면 백엔드 FD와 Content-Type·전달 대상 헤더·동반 본문의 소유권이 공급자로 넘어가며,
 * 실패하면 아무 소유권도 바꾸지 않으므로 호출자가 FD를 닫는다. */
static int prepare_backend_body(int backend_fd, const struct http_response_headers *parsed,
                                  struct http_slice prefetched_body, struct router_response *response) {
    /* 304는 표현 메타데이터용 Content-Length를 가질 수 있지만 본문은 없다. */
    size_t body_length = parsed->status == 304 ? 0
                       : parsed->has_content_length ? parsed->content_length : 0;
    if (prefetched_body.length > body_length) prefetched_body.length = body_length;

    static const char default_type[] = "application/octet-stream";
    size_t type_length = parsed->content_type.length ? parsed->content_type.length
                                                     : sizeof(default_type) - 1;

    /* 전달 대상 헤더는 원본 순서와 다중 줄(Set-Cookie 등)을 유지한 채 필요한 길이를 먼저 잰다. */
    size_t extra_length = 0;
    struct http_slice cursor = parsed->headers;
    struct http_slice name, value;
    while (http_header_next(&cursor, &name, &value)) {
        if (should_skip_backend_response_header(parsed->headers, name)) continue;
        extra_length += name.length + 2 + value.length + 2;   /* ": " + "\r\n" */
    }

    struct proxy_backend_body *body = malloc(sizeof(*body) + type_length + 1 + extra_length +
                                             prefetched_body.length);
    if (!body) return 500;

    char *storage = body->storage;
    if (parsed->content_type.length) memcpy(storage, parsed->content_type.data, type_length);
    else memcpy(storage, default_type, type_length);
    storage[type_length] = '\0';

    unsigned char *extra_headers = (unsigned char *)storage + type_length + 1;
    struct header_output out = {(char *)extra_headers, extra_length, 0};
    cursor = parsed->headers;
    while (http_header_next(&cursor, &name, &value)) {
        if (should_skip_backend_response_header(parsed->headers, name)) continue;
        if (!append_header_bytes(&out, name) ||
            !append_header_bytes(&out, http_slice_from_string(": ")) ||
            !append_header_bytes(&out, value) ||
            !append_header_bytes(&out, http_slice_from_string("\r\n"))) {
            free(body);
            return 500;
        }
    }

    *body = (struct proxy_backend_body){
        .backend_fd = backend_fd,
        .unread_length = body_length - prefetched_body.length,
        .prefetched_length = prefetched_body.length,
        .prefetched_sent = 0,
        .content_type = storage,
        .prefetched = extra_headers + extra_length
    };
    if (prefetched_body.length) {
        memcpy(body->prefetched, prefetched_body.data, prefetched_body.length);
    }

    *response = (struct router_response){
        .status = parsed->status,
        .content_type = body->content_type,
        .length = body_length,
        .headers = extra_length ? extra_headers : NULL,
        .headers_length = extra_length,
        .body_provider = {
            .next_func = proxy_backend_body_next,
            .cleanup_func = proxy_backend_body_cleanup,
            .state = body
        }
    };
    return 0;
}

/* ASCII 영문 대소문자를 무시하고 두 슬라이스가 같은지 비교한다. */
static bool header_names_equal_ignore_case(struct http_slice a, struct http_slice b) {
    if (a.length != b.length) return false;

    for (size_t i = 0; i < a.length; ++i) {
        unsigned char x = a.data[i], y = b.data[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}

/* NUL 종단 문자열의 strlen() 길이를 빌려 읽기 전용 슬라이스를 만든다. */
static struct http_slice http_slice_from_string(const char *text) {
    return (struct http_slice){(const unsigned char *)text, strlen(text)};
}

/* 연결에 종속되어 다음 연결로 넘기면 안 되는 hop-by-hop 헤더 이름인지 판별한다. */
static bool is_hop_by_hop_header(struct http_slice name) {
    static const char *const names[] = {
        "Connection", "Proxy-Connection", "Keep-Alive", "TE", "Trailer", "Upgrade",
        "Proxy-Authorization", "Proxy-Authenticate", "Transfer-Encoding"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (header_names_equal_ignore_case(name, http_slice_from_string(names[i]))) return true;
    }
    return false;
}

/* 요청에서 원본 줄을 복사하지 않을 헤더인지 판별한다: hop-by-hop + 프록시가 재작성하는 필드. */
static bool should_skip_original_request_header(struct http_slice name) {
    if (is_hop_by_hop_header(name)) return true;
    return header_names_equal_ignore_case(name, http_slice_from_string("Content-Length")) ||
           header_names_equal_ignore_case(name, http_slice_from_string("Expect"));
}

/* 응답에서 클라이언트로 그대로 넘기지 않을 헤더인지 판별한다.
 * hop-by-hop, Connection이 지목한 필드, 프록시가 다시 쓰는 Content-Length·Content-Type을 뺀다. */
static bool should_skip_backend_response_header(struct http_slice headers,
                                                 struct http_slice name) {
    if (is_hop_by_hop_header(name)) return true;
    if (connection_lists_header(headers, name)) return true;
    return header_names_equal_ignore_case(name, http_slice_from_string("Content-Length")) ||
           header_names_equal_ignore_case(name, http_slice_from_string("Content-Type"));
}

/* output 잔여 용량 내일 때만 value를 복사한다(NUL 미추가, 초과 시 미수정 후 false). */
static bool append_header_bytes(struct header_output *output, struct http_slice value) {
    if (value.length > output->capacity - output->used) return false;
    if (value.length) memcpy(output->data + output->used, value.data, value.length);
    output->used += value.length;
    return true;
}

/* 검증된 request로 백엔드용 HTTP/1.1 시작 줄과 헤더(\r\n\r\n 포함, 바디 제외)를 output에 직렬화한다.
 * validate_proxy_headers()(400) 후 omit/Connection 지명 필드를 빼고, Via·Connection: close·정규화 Content-Length를 추가한다.
 * 성공 시 *length에 기록 바이트 수(<= capacity)를 담고 0을 반환하며, 400 또는 용량 초과(500) 시 *length = 0을 보장한다. */
int build_proxy_request_headers(const struct http_request *request,
                                char *output, size_t capacity, size_t *length) {
    *length = 0;
    int status = validate_proxy_headers(request);
    if (status) return status;

    struct header_output out = {output, capacity, 0};
    if (!append_header_bytes(&out, request->method) || !append_header_bytes(&out, http_slice_from_string(" ")) ||
        !append_header_bytes(&out, request->target) || !append_header_bytes(&out, http_slice_from_string(" HTTP/1.1\r\n"))) return 500;

    struct http_slice cursor = request->headers;
    struct http_slice name, value;
    while (http_header_next(&cursor, &name, &value)) {
        if (should_skip_original_request_header(name) || connection_lists_header(request->headers, name)) continue;
        if (!append_header_bytes(&out, name) || !append_header_bytes(&out, http_slice_from_string(": ")) ||
            !append_header_bytes(&out, value) || !append_header_bytes(&out, http_slice_from_string("\r\n"))) return 500;
    }

    if (!append_header_bytes(&out, http_slice_from_string("Via: 1.1 jungle-proxy\r\nConnection: close\r\n"))) return 500;
    if (request->has_content_length) {
        char field[sizeof("Content-Length: \r\n") + 3 * sizeof(size_t)];
        int count = snprintf(field, sizeof(field), "Content-Length: %zu\r\n", request->content_length);
        if (count < 0 || (size_t)count >= sizeof(field) ||
            !append_header_bytes(&out, (struct http_slice){(const unsigned char *)field, (size_t)count})) return 500;
    }
    if (!append_header_bytes(&out, http_slice_from_string("\r\n"))) return 500;

    *length = out.used;
    return 0;
}
