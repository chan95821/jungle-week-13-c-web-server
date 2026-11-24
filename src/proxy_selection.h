#ifndef PROXY_SELECTION_H
#define PROXY_SELECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* 조회 그룹의 순번과 서버별 제외 만료 시각이다. 저장소는 proxy_config가 소유하고
 * 선택 정책 함수만 읽고 갱신하며, 공통 프록시는 내용을 해석하지 않는다. */
struct proxy_group_state {
    size_t next;
    struct timespec *excluded_until;
};

/* 이번 요청에서 아직 시도하지 않은 후보에 제외 기간과 라운드 로빈을 적용해 하나를 고른다.
 * 0: 선택 완료, 502: 선택 가능한 후보 없음, 500: 내부 오류. 연결·재시도·FD는 다루지 않는다. */
int select_available_round_robin_backend(struct proxy_group_state *group,
                                         const bool *available_for_this_request,
                                         size_t count, size_t *selected);

/* 연결 시도 결과를 그룹 상태에 반영한다. 성공은 제외 해제, 502·504는 일시 제외,
 * 그 밖의 오류는 상태 유지다. 0 또는 기록 실패 500을 반환하며 FD를 받거나 닫지 않는다. */
int record_backend_connection_result(struct proxy_group_state *group, size_t selected, int status);

/* 정책 내부에서 쓰는 순수 라운드 로빈. eligible[0..count)에서 *next부터 순환 탐색해
 * 첫 true 인덱스를 *selected에 넣고 *next를 전진시킨다(없으면 false, 상태 변경 없음). */
bool select_round_robin_backend(const bool *eligible, size_t count,
                                size_t *next, size_t *selected);

#endif
