#define _POSIX_C_SOURCE 200809L
#include "../../src/proxy_config.h"
#include "../../src/proxy_selection.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 임시 설정 파일을 만들고 내용을 쓴 뒤 경로를 유지한다.
 * mkstemp는 템플릿을 실제 파일명으로 바꾸므로 호출마다 템플릿을 새로 만든다. */
static const char *write_config(const char *text) {
    static char path[64];
    snprintf(path, sizeof(path), "/tmp/iter4-routes-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    size_t length = strlen(text);
    assert(write(fd, text, length) == (ssize_t)length);
    assert(close(fd) == 0);
    return path;
}

/* 설정 파일이 없을 때 제출 1 + 조회 2 기본 구성과 정책 포인터가 준비되는지 확인한다. */
static void loads_builtin_defaults(void) {
    assert(proxy_config_init(NULL) == 0);
    const struct proxy_upstream *submission = proxy_config_submission();
    assert(submission->backend_count == 1 && submission->max_attempts == 1);
    assert(submission->select_backend == NULL && submission->group_state == NULL);
    assert(strcmp(submission->backends[0].host, "127.0.0.1") == 0);
    assert(submission->backends[0].port == 8001);

    const struct proxy_upstream *lookup = proxy_config_lookup();
    assert(lookup->backend_count == 2 && lookup->max_attempts == 2);
    assert(lookup->backends[0].port == 8002 && lookup->backends[1].port == 8003);
    assert(lookup->select_backend == select_available_round_robin_backend);
    assert(lookup->on_connection_result == record_backend_connection_result);
    assert(lookup->group_state != NULL && lookup->group_state->excluded_until != NULL);
}

/* 주석·빈 줄·IPv6 대괄호 표기를 허용하고 지정한 주소를 그대로 확정하는지 확인한다. */
static void loads_file_addresses(void) {
    const char *path = write_config(
        "# 개인 검증용 라우팅 설정\n"
        "\n"
        "submit 127.0.0.1:9001\n"
        "lookup   127.0.0.1:9002  \n"
        "lookup [::1]:9003\n");
    assert(proxy_config_init(path) == 0);
    assert(unlink(path) == 0);

    const struct proxy_upstream *submission = proxy_config_submission();
    assert(submission->backend_count == 1 && submission->backends[0].port == 9001);

    const struct proxy_upstream *lookup = proxy_config_lookup();
    assert(lookup->backend_count == 2);
    assert(strcmp(lookup->backends[0].host, "127.0.0.1") == 0 && lookup->backends[0].port == 9002);
    assert(strcmp(lookup->backends[1].host, "::1") == 0 && lookup->backends[1].port == 9003);
}

/* 형식 오류는 실패로 끝나고, 실패한 호출이 직전에 확정된 설정을 바꾸지 않아야 한다. */
static void rejects_invalid_configs(void) {
    assert(proxy_config_init(NULL) == 0);
    const struct proxy_upstream *before = proxy_config_submission();

    static const char *const invalid[] = {
        "lookup 127.0.0.1:8002\n",                                   /* submit 없음 */
        "submit 127.0.0.1:8001\n",                                   /* lookup 없음 */
        "submit 127.0.0.1:8001\nsubmit 127.0.0.1:8002\nlookup 127.0.0.1:8003\n",
        "submit 127.0.0.1:8001\nlookup 127.0.0.1:8002\nmanagement 127.0.0.1:9000\n",
        "submit 127.0.0.1:8001\nlookup 127.0.0.1:99999\n",
        "submit 127.0.0.1:8001\nlookup localhost:8002\n",
        "submit 127.0.0.1:8001\nlookup 127.0.0.1:8002 extra\n",
        "submit 127.0.0.1:8001\nlookup [::1:8002\n",
        "submit\nlookup 127.0.0.1:8002\n",
        "submit 127.0.0.1:8001\nlookup 127.0.0.1:8002\n"
        "lookup 127.0.0.1:8003\nlookup 127.0.0.1:8004\nlookup 127.0.0.1:8005\n"
        "lookup 127.0.0.1:8006\nlookup 127.0.0.1:8007\nlookup 127.0.0.1:8008\n"
        "lookup 127.0.0.1:8009\nlookup 127.0.0.1:8010\n"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const char *path = write_config(invalid[i]);
        assert(proxy_config_init(path) != 0);
        assert(unlink(path) == 0);
        assert(proxy_config_submission() == before);
        assert(proxy_config_submission()->backend_count == 1);
        assert(proxy_config_submission()->backends[0].port == 8001);
    }

    assert(proxy_config_init("/tmp/iter4-routes-does-not-exist") != 0);
    assert(proxy_config_submission() == before);
}

/* 기본값·파일 설정·형식 오류의 확정 규칙을 확인한다. */
int main(void) {
    loads_builtin_defaults();
    loads_file_addresses();
    rejects_invalid_configs();
    puts("[PASS] test_proxy_config");
    return 0;
}
