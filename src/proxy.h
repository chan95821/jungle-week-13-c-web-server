#ifndef PROXY_H
#define PROXY_H

#include "router.h"

#include <stdint.h>

/* 단일 백엔드 대상. host 문자열은 proxy_config의 확정 저장소를 빌리며 호출자가 해제하지 않는다. */
struct proxy_backend {
    const char *host;
    uint16_t port;
};

/* 선택 정책이 정의·관리하는 지속 상태. 공통 프록시는 내용에 접근하지 않는다. */
struct proxy_group_state;

/* 라우트 핸들러가 공통 프록시에 넘기는 업스트림 목록·시도 상한·선택기·상태 참조. */
struct proxy_upstream {
    const struct proxy_backend *backends;
    size_t backend_count;
    size_t max_attempts;
    /* 0: 후보 반환, 502: 후보 없음, 500: 정책 내부 오류. */
    int (*select_backend)(struct proxy_group_state *group,
                           const bool *available_for_this_request, size_t count,
                           size_t *selected);
    /* 연결 결과로 정책 상태 갱신. 0: 완료, 500: 기록 실패. FD는 전달하지 않는다. */
    int (*on_connection_result)(struct proxy_group_state *group, size_t selected, int status);
    struct proxy_group_state *group_state;
};

/* 한 서버에 연결한다. 성공 시 0과 호출자 소유 FD, 실패 시 오류 상태와 FD -1을 반환한다.
 * 후보 선택·재시도·HTTP 송신은 하지 않으며, 실패한 시도의 소켓은 내부에서 정리한다.
 * 숫자 IPv4/IPv6만 지원한다. 설정·로컬 자원 오류 500, 원격 연결 실패 502, 연결 시간 초과 504다. */
int connect_backend(const struct proxy_backend *backend, int *backend_fd);

/* Connection 옵션에 보호 필드(Host, Content-Length, Transfer-Encoding)가 있으면 400, 없으면 0(I/O 없음). */
int validate_proxy_headers(const struct http_request *request);
/* 해석한 응답이 이번 중계 범위에 속하면 0, 아니면 502. 입력과 FD는 변경하지 않는다. */
int validate_backend_response_headers(const struct http_response_headers *response);

/* 검증된 headers의 모든 Connection 줄에서 field 지명 여부를 대소문자 무시로 판별한다. */
bool connection_lists_header(struct http_slice headers, struct http_slice field);

/* 백엔드용 HTTP/1.1 시작 줄·헤더 블록(\r\n\r\n 포함, 바디 제외)을 output에 직렬화한다. 오류(400/500) 시 *length = 0. */
int build_proxy_request_headers(const struct http_request *request,
                                char *output, size_t capacity, size_t *length);

/* 공통 프록시 응답 준비 진입점. 요청 헤더 직렬화 → 연결 확보 → 요청 송신 →
 * 응답 헤더 수신·검증 → 본문 공급자 준비 순으로 response를 채운다.
 * 성공 시 백엔드 FD는 response.body_provider.cleanup_func가 정리하고, 실패 시 이 함수가 닫는다. */
void prepare_proxy_response(const struct http_request *request,
                            const struct proxy_upstream *upstream,
                            struct router_response *response);

#endif
