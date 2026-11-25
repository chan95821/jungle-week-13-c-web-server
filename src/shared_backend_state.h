#ifndef SHARED_BACKEND_STATE_H
#define SHARED_BACKEND_STATE_H

#include "distribution_policy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

enum { SHARED_BACKEND_COUNT = 3, SHARED_BACKEND_VERSION = 4 };
#define SHARED_BACKEND_MAGIC UINT64_C(0x4a57454253485231)
enum backend_worker_lifecycle { BACKEND_WORKER_EMPTY, BACKEND_WORKER_ACTIVE, BACKEND_WORKER_RETIRING, BACKEND_WORKER_RETIRED };
enum backend_transaction_phase { BACKEND_TX_IDLE, BACKEND_TX_PREPARED, BACKEND_TX_COMMITTED };
enum backend_reservation_state { BACKEND_RESERVATION_FREE, BACKEND_RESERVATION_ACTIVE };

enum backend_state_check {
    BACKEND_CHECK_NONE, BACKEND_CHECK_MUTEX, BACKEND_CHECK_HEADER,
    BACKEND_CHECK_JOURNAL, BACKEND_CHECK_RESERVATION, BACKEND_CHECK_CONTRIBUTION,
    BACKEND_CHECK_TOTAL, BACKEND_CHECK_WORKER, BACKEND_CHECK_FATAL
};
enum backend_recovery_stage {
    BACKEND_STAGE_LOCK, BACKEND_STAGE_HEADER, BACKEND_STAGE_JOURNAL,
    BACKEND_STAGE_RESTORE_RESERVATION, BACKEND_STAGE_RESTORE_WORKER,
    BACKEND_STAGE_RESTORE_TOTAL, BACKEND_STAGE_RESTORE_TURN,
    BACKEND_STAGE_VALIDATE, BACKEND_STAGE_CONSISTENT, BACKEND_STAGE_UPDATE
};
struct backend_state_diagnostic {
    uint32_t worker_index;
    uint64_t worker_generation;
    enum backend_state_check failed_check;
    unsigned int recovery_phase;
    unsigned int recovery_stage;
};

enum worker_failure_publication { WORKER_FAILURE_EMPTY, WORKER_FAILURE_WRITING, WORKER_FAILURE_READY };
enum worker_failure_reason { WORKER_FAILURE_SHARED_STATE = 1 };
struct worker_failure_report {
    uint32_t reporter_index;
    uint32_t reason;
    uint64_t reporter_generation;
    struct backend_state_diagnostic diagnostic;
};
/* One writer per armed worker generation; parent reads only after READY. */
struct shared_worker_failure {
    atomic_uint publication;
    uint32_t reserved;
    atomic_uint_fast64_t generation;
    struct worker_failure_report report;
};
/* Fixed-size slots; neither socket FDs nor process-local pointers are shared. */
struct backend_worker_state {
    uint64_t generation;
    uint64_t outstanding[SHARED_BACKEND_COUNT];
    uint32_t state;
    uint32_t reserved;
};

struct backend_request_reservation {
    uint64_t request_generation;
    uint64_t worker_generation;
    uint32_t worker_index;
    uint32_t backend_index;
    uint32_t state;
    uint32_t reserved;
};

/* One mutex serializes updates, so one bounded before-image record suffices.
 * PREPARED rolls back; COMMITTED preserves writes. Checksum is diagnostic only. */
struct backend_recovery_record {
    atomic_uint phase;
    uint32_t worker_index;
    uint32_t reservation_index;
    uint32_t reserved;
    uint64_t previous_query_turn;
    uint32_t previous_query_selections;
    uint32_t policy_reserved;
    struct distribution_timing previous_timing[SHARED_BACKEND_COUNT];
    uint64_t previous_outstanding[SHARED_BACKEND_COUNT];
    struct backend_worker_state previous_worker;
    struct backend_request_reservation previous_reservation;
    uint64_t checksum;
};

struct shared_backend_header {
    pthread_mutex_t mutex;
    uint64_t magic;
    uint32_t version;
    uint32_t worker_capacity;
    uint32_t reservation_capacity;
    uint32_t reserved;
    uint64_t next_query_turn;
    uint32_t query_selections;
    uint32_t policy_reserved;
    struct distribution_timing timing[SHARED_BACKEND_COUNT];
    uint64_t outstanding[SHARED_BACKEND_COUNT];
    atomic_uint fatal;
    struct backend_recovery_record recovery;
};

struct shared_backend_layout {
    size_t worker_offset;
    size_t reservation_offset;
    size_t failure_offset;
    size_t mapping_bytes;
    uint32_t worker_capacity;
    uint32_t reservation_capacity;
};

/* Process-local owner/view. Never copy this object into shared memory.
 * Children may borrow the mapping after fork; only the parent destroys the
 * mutex after all users have exited. Runtime capacities have no defaults here. */
struct shared_backend_memory {
    struct shared_backend_header *header;
    struct backend_worker_state *workers;
    struct backend_request_reservation *reservations;
    struct shared_worker_failure *worker_failures;
    struct shared_backend_layout layout;
    bool mutex_initialized;
    bool lock_held;
    uint64_t *validation_counts; /* Process-local scratch, allocated before fork. */
    uint32_t diagnostic_worker;
    uint64_t diagnostic_generation;
    unsigned int recovery_stage;
};

/* Plan without allocating. All capacities and total bytes are startup bounds.
 * Fail with EINVAL/EOVERFLOW/E2BIG; zero output on failure. */
int plan_shared_backend_memory(size_t workers, size_t reservations,
                               size_t memory_budget,
                               struct shared_backend_layout *layout);

/* Initialize an empty owner before fork. On error, no mapping is published.
 * errno includes pthread attribute/init failures, without fallback. */
int create_shared_backend_memory(size_t workers, size_t reservations,
                                 size_t memory_budget,
                                 struct shared_backend_memory *memory);

/* Parent only, after all access has stopped. On destroy/unmap failure keep
 * ownership so the caller can report/handle the failure. Empty is a no-op. */
int destroy_shared_backend_memory(struct shared_backend_memory *memory);

/* No HTTP algorithm here: caller chooses while holding this same lock. */
struct backend_request_token {
    uint32_t worker_index;
    uint32_t reservation_index;
    uint64_t worker_generation;
    uint64_t request_generation;
};


/* trylock never waits. EBUSY means caller must defer, not spin in its loop.
 * EOWNERDEAD is repaired here. Fatal diagnostic => caller must notify parent
 * and stop serving; parent-wide shutdown transport is integrated separately. */
int try_lock_shared_backend_state(struct shared_backend_memory *memory,
                                  struct backend_state_diagnostic *diagnostic);
int unlock_shared_backend_state(struct shared_backend_memory *memory);
int validate_backend_reservations_locked(struct shared_backend_memory *memory,
                                         struct backend_state_diagnostic *diagnostic);
int activate_backend_worker_locked(struct shared_backend_memory *memory,
                                   uint32_t worker_index, uint64_t *generation,
                                   struct backend_state_diagnostic *diagnostic);
int reserve_backend_request_locked(struct shared_backend_memory *memory,
                                    uint32_t worker_index, uint64_t worker_generation,
                                    uint32_t reservation_index, uint32_t backend_index,
                                    uint64_t next_query_turn,
                                    struct backend_request_token *token,
                                    struct backend_state_diagnostic *diagnostic);
/* 0: released, 1: stale/already released (no change), -1: error. */
int complete_backend_request_locked(struct shared_backend_memory *memory,
                                     const struct backend_request_token *token,
                                     struct backend_state_diagnostic *diagnostic);
/* Select query A/B (backend index 1/2) and reserve in the same journal commit. */
int select_and_reserve_query_locked(struct shared_backend_memory *memory,
    enum distribution_policy policy,uint32_t worker,uint64_t generation,uint32_t slot,
    struct backend_request_token *token,uint32_t *backend,struct backend_state_diagnostic *diagnostic);
int complete_backend_request_observed_locked(struct shared_backend_memory *memory,
    const struct backend_request_token *token,const struct distribution_observation *sample,
    struct backend_state_diagnostic *diagnostic);
/* Parent only after waitpid confirmed death; generation mismatch is a no-op.
 * Bounded by reservation capacity, repeatable after interrupted recovery. */
int reclaim_backend_worker_locked(struct shared_backend_memory *memory,
                                   uint32_t worker_index, uint64_t generation,
                                   struct backend_state_diagnostic *diagnostic);
/* Format outside lock; fixed numeric fields only, no shared strings/body. */
int format_backend_state_diagnostic(char *buffer, size_t capacity,
                                    const struct backend_state_diagnostic *diagnostic);

#ifdef SHARED_BACKEND_FAULT_TEST
/* Test-only checkpoints: no hook/state in the production compilation. */
enum backend_fault_step {
    BACKEND_FAULT_JOURNAL_FILLED = 1, BACKEND_FAULT_PREPARED,
    BACKEND_FAULT_RESERVATION_WRITTEN, BACKEND_FAULT_WORKER_WRITTEN,
    BACKEND_FAULT_TOTAL_WRITTEN, BACKEND_FAULT_TURN_WRITTEN,
    BACKEND_FAULT_COMMITTED, BACKEND_FAULT_IDLE,
    BACKEND_FAULT_RESTORE_RESERVATION, BACKEND_FAULT_RESTORE_WORKER,
    BACKEND_FAULT_RESTORE_TOTAL, BACKEND_FAULT_RESTORE_TURN,
    BACKEND_FAULT_RECOVERY_VALIDATED, BACKEND_FAULT_RECOVERY_IDLE,
    BACKEND_FAULT_CONSISTENT,
    BACKEND_FAULT_TIMING_WRITTEN, BACKEND_FAULT_RESTORE_TIMING,
    BACKEND_FAULT_TIMING_REPRESENTATIVE, BACKEND_FAULT_RESTORE_TIMING_REPRESENTATIVE,
    BACKEND_FAULT_QUERY_CURSOR_WRITTEN,
    BACKEND_FAULT_WORKER_GENERATION_WRITTEN
};
void shared_backend_fault_step(enum backend_fault_step step);
#endif

#endif
