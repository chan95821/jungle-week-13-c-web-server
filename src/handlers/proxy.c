#include "handlers.h"
#include "../proxy.h"
#include "../proxy_config.h"

/* GET /api/hello 라우트 핸들러. 부모 이슈 #4가 정한 중계 확인용 경로이며,
 * 이번 구성의 단일 백엔드(제출 서버)로 전달해 응답이 그대로 돌아오는지 확인한다. */
void prepare_hello_proxy(const struct http_request *request,
                         struct router_response *response) {
    prepare_proxy_response(request, proxy_config_submission(), response);
}

/* POST /api/submissions 라우트 핸들러. 설정이 확정한 제출 백엔드 한 곳을
 * prepare_proxy_response()에 넘긴다. 공통 프록시는 경로로 목적지를 고르지 않는다. */
void prepare_submission_proxy(const struct http_request *request,
                              struct router_response *response) {
    prepare_proxy_response(request, proxy_config_submission(), response);
}

/* GET /api/submission-status 라우트 핸들러. 설정이 확정한 조회 서버 그룹과
 * 선택·제외 정책, 요청 간 유지되는 그룹 상태를 공통 프록시에 넘긴다. */
void prepare_submission_status_proxy(const struct http_request *request,
                                     struct router_response *response) {
    prepare_proxy_response(request, proxy_config_lookup(), response);
}
