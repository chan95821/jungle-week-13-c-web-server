#ifndef BACKEND_REQUEST_LIFETIME_H
#define BACKEND_REQUEST_LIFETIME_H
#include "shared_backend_state.h"

enum backend_request_phase { BACKEND_REQUEST_IDLE, BACKEND_REQUEST_ACTIVE,
    BACKEND_REQUEST_RELEASE_PENDING, BACKEND_REQUEST_RELEASED };
enum backend_request_outcome { BACKEND_OUTCOME_NONE, BACKEND_OUTCOME_COMPLETE,
    BACKEND_OUTCOME_FAILED, BACKEND_OUTCOME_CANCELLED };
/* Worker connection owns this stable object/token; no FD, allocation or timer.
 * Only reset after confirmed release. Parent reclaims a dead worker's token
 * from shared ledger; it never accesses this private object. */
struct backend_request_lifetime {
    struct backend_request_token token;
    struct distribution_observation observation;
    struct timespec started_at;
    bool clock_started;
    enum backend_request_phase phase;
    enum backend_request_outcome outcome;
};
/* Caller holds same shared lock used to SELECT backend, after request body
 * completion and before connect. Does not acquire/release lock or choose policy.
 * Leaves lifetime unchanged on reservation failure. */
int begin_backend_request_locked(struct shared_backend_memory *memory,
    struct backend_request_lifetime *lifetime, uint32_t worker, uint64_t generation,
    uint32_t slot, uint32_t backend, uint64_t next_turn,
    struct backend_state_diagnostic *diagnostic);
int begin_selected_query_request_locked(struct shared_backend_memory *memory,
    struct backend_request_lifetime *lifetime,enum distribution_policy policy,
    uint32_t worker,uint64_t generation,uint32_t slot,uint32_t *backend,
    struct backend_state_diagnostic *diagnostic);
/* CLOCK_MONOTONIC timestamps supplied outside shared lock. Start immediately
 * before connect, finish at framing completion/failure, not at release lock. */
int start_backend_request_clock(struct backend_request_lifetime *lifetime,const struct timespec *now);
int finish_backend_request_clock(struct backend_request_lifetime *lifetime,
    const struct timespec *now,enum backend_request_outcome outcome);
/* Caller invokes COMPLETE only after final response framing fully validates:
 * fixed length / zero-body final headers / chunk end+trailers / valid EOF.
 * Buffered client output may remain. Partial framing/1xx/half-close is not
 * completion. Failure/cancel caller closes backend FD outside shared lock.
 * 0 first transition, 1 already finished (first outcome retained), -1 invalid. */
int mark_backend_request_finished(struct backend_request_lifetime *lifetime,
                                  enum backend_request_outcome outcome);
/* 0 confirmed released/no-op, 1 EBUSY pending, -1 error; one trylock, no retry.
 * Fatal/error retains token/phase; caller initiates existing failure shutdown.
 * Caller owns retry scheduling; parent retry policy is not implied here. */
int try_release_backend_request(struct shared_backend_memory *memory,
    struct backend_request_lifetime *lifetime, struct backend_state_diagnostic *diagnostic);
/* EBUSY while active/pending; preserves token until release confirmed. */
int reset_backend_request_lifetime(struct backend_request_lifetime *lifetime);
#endif
