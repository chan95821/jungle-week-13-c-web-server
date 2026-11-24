#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/handlers/handlers.h"

/* Verify route-specific header checks without changing existing route semantics. */
static void validates_only_matched_proxy_routes(void) {
    static const struct {
        const char *method, *target, *option;
        int status;
    } cases[] = {
        {"POST", "/api/submissions", "hOsT", 400},
        {"GET", "/api/submission-status?q=1", "content-LENGTH", 400},
        {"GET", "/api/submission-status", "Transfer-Encoding", 400},
        {"GET", "/api/submission-status", "X-Demo", 0},
        {"GET", "/api/submissions", "Host", 405},
        {"GET", "/api/submissions/extra", "Host", 404},
        {"GET", "/hello", "Host", 0},
        {"POST", "/echo", "Content-Length", 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char wire[512];
        int count = snprintf(wire, sizeof(wire),
                             "%s %s HTTP/1.1\r\nHost: demo.example\r\n"
                             "X-Demo: value\r\nConnection: close\r\n"
                             "Connection: %s\r\nExpect: 100-continue\r\n"
                             "Content-Length: 10\r\n\r\n",
                             cases[i].method, cases[i].target, cases[i].option);
        assert(count > 0 && (size_t)count < sizeof(wire));
        struct http_request request;
        assert(http_parse_request((const unsigned char *)wire, (size_t)count, &request) == 0);
        assert(request.body.data == NULL && request.body.length == 0);

        router_handler handler = prepare_hello;
        const char *allow = NULL;
        assert(http_select_handler(&request, &handler, &allow) == cases[i].status);
        assert((handler != NULL) == (cases[i].status == 0));
        if (cases[i].status == 405) assert(allow && strcmp(allow, "POST") == 0);
        else assert(allow == NULL);
    }
}

/* Run pre-body route validation independently of backend connections. */
int main(void) {
    validates_only_matched_proxy_routes();
    puts("[PASS] test_proxy_validation");
    return 0;
}
