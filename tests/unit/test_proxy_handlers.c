#include <assert.h>
#include <stdio.h>

#include "../../src/handlers/handlers.h"
#include "../../src/proxy.h"
#include "../../src/proxy_config.h"
#include "../../src/proxy_selection.h"

static size_t calls;
static const struct http_request *seen_request;
static const struct proxy_upstream *seen_upstream;
static struct router_response *seen_response;

/* Capture delegation without linking the not-yet-implemented HTTP relay. */
void prepare_proxy_response(const struct http_request *request,
                            const struct proxy_upstream *upstream,
                            struct router_response *response) {
    ++calls;
    seen_request = request;
    seen_upstream = upstream;
    seen_response = response;
}

/* Check that each real handler delegates its own settings and the original objects. */
int main(void) {
    static const unsigned char submission_wire[] =
        "POST /api/submissions?course=os HTTP/1.1\r\nHost: demo.example\r\nContent-Length: 0\r\n\r\n";
    static const unsigned char status_wire[] =
        "GET /api/submission-status HTTP/1.1\r\nHost: demo.example\r\n\r\n";
    struct http_request submission_request, status_request;
    assert(http_parse_request(submission_wire, sizeof(submission_wire) - 1, &submission_request) == 0);
    assert(http_parse_request(status_wire, sizeof(status_wire) - 1, &status_request) == 0);
    /* 핸들러는 설정이 확정한 업스트림을 넘기므로 기본값으로 초기화한다. */
    assert(proxy_config_init(NULL) == 0);

    router_handler submission_handler, status_handler;
    const char *allow;
    assert(http_select_handler(&submission_request, &submission_handler, &allow) == 0);
    assert(submission_handler == prepare_submission_proxy && allow == NULL);
    assert(http_select_handler(&status_request, &status_handler, &allow) == 0);
    assert(status_handler == prepare_submission_status_proxy && allow == NULL);

    struct http_request wrong = submission_request;
    wrong.method = status_request.method;
    router_handler rejected;
    assert(http_select_handler(&wrong, &rejected, &allow) == 405);
    assert(rejected == NULL && allow && http_slice_equal(submission_request.method, allow));
    wrong.path = (struct http_slice){(const unsigned char *)"/api/submissions/extra",
                                     sizeof("/api/submissions/extra") - 1};
    assert(http_select_handler(&wrong, &rejected, &allow) == 404);
    assert(rejected == NULL && allow == NULL);

    /* 부모 이슈 #4의 /api/hello 완료 조건은 전용 핸들러로 같은 단일 백엔드에 연결한다. */
    struct http_request hello_request = status_request;
    hello_request.path = (struct http_slice){(const unsigned char *)"/api/hello",
                                             sizeof("/api/hello") - 1};
    router_handler hello_handler;
    assert(http_select_handler(&hello_request, &hello_handler, &allow) == 0);
    assert(hello_handler == prepare_hello_proxy && allow == NULL);

    struct router_response response;
    submission_handler(&submission_request, &response);
    assert(calls == 1 && seen_upstream != NULL);
    assert(seen_upstream->backend_count == 1 && seen_upstream->max_attempts == 1);
    assert(seen_upstream->backends != NULL && seen_upstream->select_backend == NULL);
    const struct proxy_upstream *submission = seen_upstream;
    assert(seen_request == &submission_request && seen_response == &response);

    status_handler(&status_request, &response);
    assert(calls == 2 && seen_upstream != NULL && seen_upstream != submission);
    assert(seen_upstream->backend_count == 2 && seen_upstream->max_attempts == 2);
    assert(seen_upstream->select_backend == select_available_round_robin_backend);
    assert(seen_upstream->on_connection_result == record_backend_connection_result);
    assert(seen_upstream->group_state != NULL);
    assert(seen_upstream->group_state->excluded_until != NULL);
    assert(seen_request == &status_request && seen_response == &response);

    const struct proxy_upstream *status = seen_upstream;
    status->group_state->next = 1;
    status_handler(&status_request, &response);
    assert(calls == 3 && seen_upstream == status);
    assert(seen_upstream->group_state->next == 1);

    puts("[PASS] test_proxy_handlers");
    return 0;
}
