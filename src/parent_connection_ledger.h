#ifndef PARENT_CONNECTION_LEDGER_H
#define PARENT_CONNECTION_LEDGER_H
#include "distribution_policy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum parent_connection_phase { PARENT_CONNECTION_FREE, PARENT_CONNECTION_QUEUED,
                               PARENT_CONNECTION_SENDING, PARENT_CONNECTION_OWNED };
/* Parent-only accounting, no FD ownership and no I/O. Preserve tombstones. */
struct parent_connection_entry {
    uint64_t connection_id;
    uint64_t worker_generation;
    uint32_t worker_index;
    enum parent_connection_phase phase;
    bool closed_before_handoff;
    struct distribution_observation completion;
};
struct parent_connection_worker {
    uint64_t generation;
    size_t outstanding;
    bool active;
    struct distribution_timing timing;
};
struct parent_connection_ledger {
    struct parent_connection_entry *entries;
    struct parent_connection_worker *workers;
    struct distribution_candidate *selection_candidates;
    size_t capacity, worker_capacity, worker_limit, total, queued;
    struct distribution_turn selection_turn;
};
/* Zero-initialize owners. All allocations fixed at creation; destroy owns no FD. */
int create_parent_connection_ledger(size_t capacity, size_t workers, size_t worker_limit,
                                    struct parent_connection_ledger *ledger);
void destroy_parent_connection_ledger(struct parent_connection_ledger *ledger);
int activate_parent_connection_worker(struct parent_connection_ledger *ledger,
                                      uint32_t worker, uint64_t generation);
int register_parent_connection(struct parent_connection_ledger *ledger, uint64_t *connection_id);
int assign_parent_connection(struct parent_connection_ledger *ledger, uint64_t connection_id,
                              uint32_t worker, uint64_t generation);
/* Caller supplies actual channel/sender availability. Parent-only calculation
 * and assignment publish cursor only after success, no allocation/I/O. */
int select_and_assign_parent_connection(struct parent_connection_ledger *ledger,
    uint64_t connection_id,enum distribution_policy policy,const bool *available,uint32_t *worker);
int apply_worker_connection_observed_closed(struct parent_connection_ledger *ledger,
    uint32_t worker,uint64_t generation,uint64_t connection_id,
    const struct distribution_observation *sample);
/* Called only after sender finished (or was aborted after reaped-worker death).
 * Early CLOSED remains counted until this handoff cleanup is finished. */
int finish_parent_connection_handoff(struct parent_connection_ledger *ledger, uint64_t connection_id);
/* Parent dispatch only: sender has not transmitted any FD byte. Drop a client
 * that closed before handoff; no redispatch and no timing sample. */
int cancel_untransferred_parent_connection(struct parent_connection_ledger *ledger,uint64_t connection_id);
int cancel_queued_parent_connection(struct parent_connection_ledger *ledger, uint64_t connection_id);
/* 0 applied, 1 stale/duplicate no-op, -1 error. Future/wrong-owner is EPROTO. */
int apply_worker_connection_closed(struct parent_connection_ledger *ledger,
                                    uint32_t worker, uint64_t generation, uint64_t connection_id);
/* Caller must confirm waitpid and abort that generation's sender first.
 * 0 reclaimed, 1 stale/already reaped, -1 error. No automatic respawn/retry. */
int reclaim_reaped_worker_connections(struct parent_connection_ledger *ledger,
                                       uint32_t worker, uint64_t generation);
int validate_parent_connection_ledger(const struct parent_connection_ledger *ledger);
#endif
