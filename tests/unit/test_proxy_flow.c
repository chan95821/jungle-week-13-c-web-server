#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/proxy.h"

/* Verify the real orchestration prepares an error response before unfinished network work. */
static void prepares_errors_in_stage_order(void) {
    static const struct {
        const char *connection;
        int expected_status;
    } cases[] = {
        {"Host", 400},
        {"close", 500}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char wire[160];
        int count = snprintf(wire, sizeof(wire),
                             "GET /api/submission-status HTTP/1.1\r\n"
                             "Host: demo.example\r\nConnection: %s\r\n\r\n",
                             cases[i].connection);
        assert(count > 0 && (size_t)count < sizeof(wire));
        struct http_request request;
        assert(http_parse_request((const unsigned char *)wire, (size_t)count, &request) == 0);

        struct router_response response;
        prepare_proxy_response(&request, NULL, &response);
        assert(response.status == cases[i].expected_status);
        assert(response.body_provider.next_func != NULL);
        assert(response.body_provider.cleanup_func == NULL);
    }
}

/* Run top-level error-path checks without replacing the proxy function with a double. */
int main(void) {
    prepares_errors_in_stage_order();
    puts("[PASS] test_proxy_flow");
    return 0;
}
