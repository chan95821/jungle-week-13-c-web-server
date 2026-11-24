#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include "../../src/proxy_selection.h"

/* 선택 정책은 요청별 시도 이력과 지속 제외 상태를 함께 적용하고 연결 결과로 상태를 갱신한다. */
static void selection_policy_owns_exclusion(void) {
    bool available[] = {true, true};
    struct timespec excluded[2] = {0};
    struct proxy_group_state group = {.excluded_until = excluded};
    size_t selected;
    assert(select_available_round_robin_backend(&group, available, 2, &selected) == 0);
    assert(selected == 0);
    assert(record_backend_connection_result(&group, selected, 502) == 0);
    assert(excluded[0].tv_sec != 0);

    assert(clock_gettime(CLOCK_MONOTONIC, &excluded[0]) == 0);
    excluded[0].tv_sec += 60;
    group.next = 0;
    assert(select_available_round_robin_backend(&group, available, 2, &selected) == 0);
    assert(selected == 1);
    available[1] = false;
    assert(select_available_round_robin_backend(&group, available, 2, &selected) == 502);

    struct timespec saved = excluded[0];
    assert(record_backend_connection_result(&group, 0, 500) == 0);
    assert(record_backend_connection_result(&group, 0, 501) == 0);
    assert(excluded[0].tv_sec == saved.tv_sec && excluded[0].tv_nsec == saved.tv_nsec);
    assert(record_backend_connection_result(&group, 0, 0) == 0);
    assert(excluded[0].tv_sec == 0 && excluded[0].tv_nsec == 0);
    assert(select_available_round_robin_backend(&group, available, 2, &selected) == 0);
    assert(selected == 0);
}

/* Verify alternation, exclusion, no eligible backend, and re-entry. */
int main(void) {
    selection_policy_owns_exclusion();
    bool eligible[] = {true, true};
    size_t next = 0, selected;

    for (size_t i = 0; i < 4; ++i) {
        assert(select_round_robin_backend(eligible, 2, &next, &selected));
        assert(selected == i % 2);
    }

    eligible[0] = false;
    assert(select_round_robin_backend(eligible, 2, &next, &selected));
    assert(selected == 1);
    eligible[1] = false;
    assert(!select_round_robin_backend(eligible, 2, &next, &selected));
    assert(!select_round_robin_backend(NULL, 0, &next, &selected));
    eligible[0] = true;
    assert(select_round_robin_backend(eligible, 2, &next, &selected));
    assert(selected == 0);

    puts("[PASS] test_proxy_selection");
    return 0;
}
