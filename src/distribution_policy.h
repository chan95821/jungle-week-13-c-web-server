#ifndef DISTRIBUTION_POLICY_H
#define DISTRIBUTION_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
enum distribution_policy { DISTRIBUTE_RR, DISTRIBUTE_LEAST_OUTSTANDING,
    DISTRIBUTE_RESPONSE_TIME, DISTRIBUTE_COMBINED };
enum distribution_sample_result { DISTRIBUTION_NO_SAMPLE, DISTRIBUTION_COMPLETED,
    DISTRIBUTION_FAILED, DISTRIBUTION_CANCELLED };
enum { DISTRIBUTION_EWMA_DIVISOR=8, DISTRIBUTION_EXPLORE_INTERVAL=32 };
#define DISTRIBUTION_FAILURE_FLOOR_US UINT64_C(1000000)
struct distribution_timing { uint64_t representative_us, last_finished_us; };
/* selections modulo 32, cursor is next search start. One owner/held shared lock. */
struct distribution_turn { uint32_t cursor, selections; };
struct distribution_candidate {
    uint64_t outstanding;
    struct distribution_timing timing;
    bool eligible;
};
struct distribution_observation {
    uint64_t elapsed_us, finished_us;
    enum distribution_sample_result result;
};
int parse_distribution_policy(const char *text, enum distribution_policy *policy);
int distribution_elapsed_us(const struct timespec *start,const struct timespec *finish,
                            struct distribution_observation *sample,enum distribution_sample_result result);
int update_distribution_timing(struct distribution_timing *timing,const struct distribution_observation *sample);
bool valid_distribution_observation(const struct distribution_observation *sample);
/* Pure calculation: output turn not published until caller's reservation commits.
 * EAGAIN no eligible candidate; no exclusion/retry/fallback policy added. */
int choose_distribution_candidate(enum distribution_policy policy,
    const struct distribution_candidate *candidates,size_t count,
    const struct distribution_turn *turn,size_t *selected,struct distribution_turn *next);
#endif
