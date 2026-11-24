#define _POSIX_C_SOURCE 200809L
#include "proxy_selection.h"

#include <stdlib.h>

/* 조회 정책의 개인 검증 시작값이며 공통 연결 흐름에는 노출하지 않는다. */
#define BACKEND_EXCLUSION_SECONDS 2

/* 호출자가 넘긴 이번 요청의 미시도 목록과 그룹의 지속 제외 상태를 합쳐 내부 임시 목록을 만들고,
 * 순수 RR 함수로 후보를 고른 뒤 그 임시 목록을 해제한다. 호출자의 목록·FD는 변경하지 않으며,
 * 그룹 순번은 선택에 성공한 경우에만 전진한다. */
int select_available_round_robin_backend(struct proxy_group_state *group,
                                         const bool *available_for_this_request,
                                         size_t count, size_t *selected) {
    if (!group || !group->excluded_until) return 500;
    if (count == 0) return 502;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 500;
    bool *eligible = calloc(count, sizeof(*eligible));
    if (!eligible) return 500;

    for (size_t i = 0; i < count; ++i) {
        struct timespec expiry = group->excluded_until[i];
        bool exclusion_expired = expiry.tv_sec < now.tv_sec ||
                                 (expiry.tv_sec == now.tv_sec && expiry.tv_nsec <= now.tv_nsec);
        eligible[i] = available_for_this_request[i] && exclusion_expired;
    }

    bool found = select_round_robin_backend(eligible, count, &group->next, selected);
    free(eligible);
    return found ? 0 : 502;
}

/* acquire_proxy_backend()가 연결 시도 직후 통보한 결과만으로 조회 그룹 상태를 갱신한다.
 * selected는 직전 선택의 유효 인덱스다. FD와 요청별 시도 이력은 소유하거나 변경하지 않는다.
 * 실패 시각 조회에 실패하면 상태를 유지하고 500을 반환한다. */
int record_backend_connection_result(struct proxy_group_state *group, size_t selected, int status) {
    if (!group || !group->excluded_until) return 500;
    if (status == 0) {
        group->excluded_until[selected] = (struct timespec){0};
        return 0;
    }
    if (status != 502 && status != 504) return 0;

    struct timespec expiry;
    if (clock_gettime(CLOCK_MONOTONIC, &expiry) < 0) return 500;
    expiry.tv_sec += BACKEND_EXCLUSION_SECONDS;
    group->excluded_until[selected] = expiry;
    return 0;
}

/* 선택 정책이 사용하는 순수 라운드로빈 함수. 제외 시각이나 연결 결과를 알지 못한다.
 * *next % count부터 최대 count칸을 순환 스캔해 첫 적격(true) 인덱스를 *selected에 넣고 *next를 다음 칸으로 갱신한다.
 * count == 0이거나 적격 항목이 없으면 *next와 *selected를 건드리지 않고 false를 반환한다. */
bool select_round_robin_backend(const bool *eligible, size_t count,
                                size_t *next, size_t *selected) {
    if (count == 0) return false;

    size_t candidate = *next % count;
    for (size_t i = 0; i < count; ++i) {
        if (eligible[candidate]) {
            *selected = candidate;
            *next = (candidate + 1) % count;
            return true;
        }
        candidate = (candidate + 1) % count;
    }

    return false;
}
