#ifndef PROXY_H
#define PROXY_H

#include "http.h"
#include <netinet/in.h>
#include <signal.h>

/* 설정과 조회 순번은 main의 수명에 속한다. 연결 버퍼·FD는 여기에 넣지 않는다.
 * 현재는 순차 서버라 순번 동기화가 필요 없다. #5에서 동시성을 도입할 때
 * 이 순번의 공유·동기화 범위를 별도로 정해야 한다. */
struct proxy_config {
    struct sockaddr_in submission;
    struct sockaddr_in status[2];
    unsigned int next_status;
};

enum proxy_route { PROXY_LOCAL, PROXY_SUBMISSION, PROXY_STATUS };

/* 직접 구현한 설정 파서. DNS 조회 없이 숫자 IPv4:port만 받는다. */
int parse_backend_address(const char *text, struct sockaddr_in *address);
/* 양수: 프록시 경로, 0: 기존 라우터, 음수: HTTP 오류 코드의 음수. */
int choose_reverse_proxy_route(const struct http_request *request, const char **allow);

/* 요청의 원본 헤더·본문을 빌려 동기적으로 중계한다.
 * 0: 응답 완료, 양수: 아직 응답 전이므로 호출자가 보낼 HTTP 오류 코드,
 * -1: 응답 송신을 시도했거나 종료 중이므로 호출자는 새 응답을 보내면 안 된다.
 * backend FD는 이 함수만 소유·close, client FD는 빌려 쓰고 닫지 않는다. */
int handle_reverse_proxy_request(
    int client, const struct http_request *request,
    const unsigned char *header, size_t header_length,
    enum proxy_route route, struct proxy_config *config,
    const volatile sig_atomic_t *running);

#endif
