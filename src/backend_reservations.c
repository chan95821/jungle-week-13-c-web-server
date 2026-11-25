#define _POSIX_C_SOURCE 200809L
#include "shared_backend_state.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifdef SHARED_BACKEND_FAULT_TEST
#define CHECKPOINT(step) shared_backend_fault_step(step)
#else
#define CHECKPOINT(step) ((void)0)
#endif

static int reject_corruption(struct shared_backend_memory *memory,
                              struct backend_state_diagnostic *diagnostic,
                              enum backend_state_check check);
static int check_header(struct shared_backend_memory *memory,
                         struct backend_state_diagnostic *diagnostic);
static int require_lock(struct shared_backend_memory *memory,
                         struct backend_state_diagnostic *diagnostic);
static uint64_t recovery_checksum(const struct backend_recovery_record *record);
static int recover_interrupted_change(struct shared_backend_memory *memory,
                                      struct backend_state_diagnostic *diagnostic);
static void prepare_change(struct shared_backend_memory *memory,
                            uint32_t worker, uint32_t reservation);
static void commit_change(struct shared_backend_memory *memory);
static int check_active_reservation(struct shared_backend_memory *memory,
                                     uint32_t index,
                                     struct backend_state_diagnostic *diagnostic);
static void change_worker_state(struct shared_backend_memory *memory,
                                uint32_t worker, uint32_t state,
                                uint64_t generation);

int try_lock_shared_backend_state(struct shared_backend_memory *memory,
                                  struct backend_state_diagnostic *diagnostic) {
    if (diagnostic) *diagnostic = (struct backend_state_diagnostic){.worker_index = UINT32_MAX};
    if (!memory || !memory->header || !memory->validation_counts) { errno = EINVAL; return -1; }
    if (memory->lock_held) { errno = EDEADLK; return -1; }
    memory->diagnostic_worker = UINT32_MAX;
    memory->diagnostic_generation = 0;
    memory->recovery_stage = BACKEND_STAGE_LOCK;
    int result = pthread_mutex_trylock(&memory->header->mutex);
    if (result && result != EOWNERDEAD) {
        if (result == ENOTRECOVERABLE) {
            reject_corruption(memory, diagnostic, BACKEND_CHECK_MUTEX);
        } else errno = result;
        return -1;
    }
    memory->lock_held = true;
    if (check_header(memory, diagnostic) < 0) goto fatal;
    if (result == EOWNERDEAD) {
        if (recover_interrupted_change(memory, diagnostic) < 0) goto fatal;
        memory->recovery_stage = BACKEND_STAGE_CONSISTENT;
        int consistent_result = pthread_mutex_consistent(&memory->header->mutex);
        if (consistent_result) {
            reject_corruption(memory, diagnostic, BACKEND_CHECK_MUTEX);
            goto fatal;
        }
        CHECKPOINT(BACKEND_FAULT_CONSISTENT);
    } else if (atomic_load_explicit(&memory->header->recovery.phase, memory_order_seq_cst) != BACKEND_TX_IDLE) {
        reject_corruption(memory, diagnostic, BACKEND_CHECK_JOURNAL);
        goto fatal;
    }
    return 0;
fatal:
    /* An inconsistent mutex is deliberately not declared consistent on failure.
     * The fatal latch forbids reuse even when the mutex was initially healthy. */
    pthread_mutex_unlock(&memory->header->mutex);
    memory->lock_held = false;
    errno = EUCLEAN;
    return -1;
}

int unlock_shared_backend_state(struct shared_backend_memory *memory) {
    if (!memory || !memory->header || !memory->lock_held) { errno = EPERM; return -1; }
    int result = pthread_mutex_unlock(&memory->header->mutex);
    if (result) { errno = result; return -1; }
    memory->lock_held = false;
    return 0;
}

int validate_backend_reservations_locked(struct shared_backend_memory *memory,
                                         struct backend_state_diagnostic *diagnostic) {
    if (require_lock(memory, diagnostic) < 0) return -1;
    memory->recovery_stage = BACKEND_STAGE_VALIDATE;
    size_t workers = memory->layout.worker_capacity;
    size_t reservations = memory->layout.reservation_capacity;
    memset(memory->validation_counts, 0, workers * SHARED_BACKEND_COUNT * sizeof(uint64_t));
    for (size_t i = 0; i < workers; ++i) {
        const struct backend_worker_state *worker = &memory->workers[i];
        memory->diagnostic_worker = (uint32_t)i;
        memory->diagnostic_generation = worker->generation;
        if (worker->state > BACKEND_WORKER_RETIRED || worker->reserved ||
            (worker->state == BACKEND_WORKER_EMPTY && worker->generation) ||
            (worker->state != BACKEND_WORKER_EMPTY && !worker->generation))
            return reject_corruption(memory, diagnostic, BACKEND_CHECK_WORKER);
    }
    for (size_t i = 0; i < reservations; ++i) {
        const struct backend_request_reservation *request = &memory->reservations[i];
        memory->diagnostic_worker = request->worker_index;
        memory->diagnostic_generation = request->worker_generation;
        if (request->state > BACKEND_RESERVATION_ACTIVE || request->reserved ||
            request->worker_index >= workers || request->backend_index >= SHARED_BACKEND_COUNT)
            return reject_corruption(memory, diagnostic, BACKEND_CHECK_RESERVATION);
        if (request->state == BACKEND_RESERVATION_ACTIVE) {
            if (check_active_reservation(memory, (uint32_t)i, diagnostic) < 0) return -1;
            ++memory->validation_counts[(size_t)request->worker_index * SHARED_BACKEND_COUNT + request->backend_index];
        }
    }
    uint64_t total[SHARED_BACKEND_COUNT] = {0};
    for (size_t i = 0; i < workers; ++i) {
        for (size_t b = 0; b < SHARED_BACKEND_COUNT; ++b) {
            memory->diagnostic_worker = (uint32_t)i;
            memory->diagnostic_generation = memory->workers[i].generation;
            uint64_t expected = memory->validation_counts[i * SHARED_BACKEND_COUNT + b];
            if (memory->workers[i].outstanding[b] != expected)
                return reject_corruption(memory, diagnostic, BACKEND_CHECK_CONTRIBUTION);
            total[b] += expected; /* At most reservation_capacity active entries. */
        }
    }
    memory->diagnostic_worker = UINT32_MAX;
    memory->diagnostic_generation = 0;
    for (size_t b = 0; b < SHARED_BACKEND_COUNT; ++b)
        if (memory->header->outstanding[b] != total[b])
            return reject_corruption(memory, diagnostic, BACKEND_CHECK_TOTAL);
    if(memory->header->next_query_turn>1 || memory->header->query_selections>=DISTRIBUTION_EXPLORE_INTERVAL || memory->header->policy_reserved)
        return reject_corruption(memory,diagnostic,BACKEND_CHECK_HEADER);
    for(size_t b=0;b<SHARED_BACKEND_COUNT;++b)
        if(!memory->header->timing[b].representative_us && memory->header->timing[b].last_finished_us)
            return reject_corruption(memory,diagnostic,BACKEND_CHECK_HEADER);
    return 0;
}

int activate_backend_worker_locked(struct shared_backend_memory *memory,
                                   uint32_t worker_index, uint64_t *generation,
                                   struct backend_state_diagnostic *diagnostic) {
    if (require_lock(memory, diagnostic) < 0) return -1;
    if (!generation || worker_index >= memory->layout.worker_capacity) { errno = EINVAL; return -1; }
    if (validate_backend_reservations_locked(memory, diagnostic) < 0) return -1;
    struct backend_worker_state *worker = &memory->workers[worker_index];
    memory->diagnostic_worker = worker_index;
    memory->diagnostic_generation = worker->generation;
    memory->recovery_stage = BACKEND_STAGE_UPDATE;
    if (worker->state != BACKEND_WORKER_EMPTY && worker->state != BACKEND_WORKER_RETIRED) {
        errno = EBUSY;
        return -1;
    }
    if (worker->generation == UINT64_MAX) { errno = EOVERFLOW; return -1; }
    change_worker_state(memory, worker_index, BACKEND_WORKER_ACTIVE, worker->generation + 1);
    *generation = worker->generation;
    return 0;
}

static int reserve_request_transaction(struct shared_backend_memory *memory,
                                    uint32_t worker_index, uint64_t worker_generation,
                                    uint32_t reservation_index, uint32_t backend_index,
                                    uint64_t next_query_turn, uint32_t next_selections,
                                    struct backend_request_token *token,
                                    struct backend_state_diagnostic *diagnostic) {
    if (require_lock(memory, diagnostic) < 0) return -1;
    if (!token || worker_index >= memory->layout.worker_capacity ||
        reservation_index >= memory->layout.reservation_capacity ||
        backend_index >= SHARED_BACKEND_COUNT || next_query_turn > 1 ||
        next_selections >= DISTRIBUTION_EXPLORE_INTERVAL) { errno = EINVAL; return -1; }
    struct backend_worker_state *worker = &memory->workers[worker_index];
    memory->diagnostic_worker = worker_index;
    memory->diagnostic_generation = worker->generation;
    memory->recovery_stage = BACKEND_STAGE_UPDATE;
    struct backend_request_reservation *request = &memory->reservations[reservation_index];
    if (worker->state > BACKEND_WORKER_RETIRED || request->state > BACKEND_RESERVATION_ACTIVE)
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_WORKER);
    if (worker->state != BACKEND_WORKER_ACTIVE || worker->generation != worker_generation || !worker_generation) {
        errno = ESTALE;
        return -1;
    }
    if (request->state != BACKEND_RESERVATION_FREE) { errno = EBUSY; return -1; }
    if (request->reserved || worker->reserved ||
        worker->outstanding[backend_index] > memory->header->outstanding[backend_index])
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_CONTRIBUTION);
    if (request->request_generation == UINT64_MAX) { errno = EOVERFLOW; return -1; }
    uint64_t total = 0;
    for (size_t b = 0; b < SHARED_BACKEND_COUNT; ++b) total += memory->header->outstanding[b];
    if (total > memory->layout.reservation_capacity)
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_TOTAL);
    if (total == memory->layout.reservation_capacity) { errno = ENOSPC; return -1; }

    prepare_change(memory, worker_index, reservation_index);
    *request = (struct backend_request_reservation){
        .request_generation = request->request_generation + 1,
        .worker_generation = worker_generation, .worker_index = worker_index,
        .backend_index = backend_index, .state = BACKEND_RESERVATION_ACTIVE
    };
    CHECKPOINT(BACKEND_FAULT_RESERVATION_WRITTEN);
    ++worker->outstanding[backend_index];
    CHECKPOINT(BACKEND_FAULT_WORKER_WRITTEN);
    ++memory->header->outstanding[backend_index];
    CHECKPOINT(BACKEND_FAULT_TOTAL_WRITTEN);
    memory->header->next_query_turn = next_query_turn;
    CHECKPOINT(BACKEND_FAULT_QUERY_CURSOR_WRITTEN);
    memory->header->query_selections = next_selections;
    CHECKPOINT(BACKEND_FAULT_TURN_WRITTEN);
    commit_change(memory);
    *token = (struct backend_request_token){worker_index, reservation_index,
                                           worker_generation, request->request_generation};
    return 0;
}

int reserve_backend_request_locked(struct shared_backend_memory *memory,
    uint32_t worker,uint64_t generation,uint32_t slot,uint32_t backend,uint64_t next_turn,
    struct backend_request_token *token,struct backend_state_diagnostic *diagnostic) {
    return reserve_request_transaction(memory,worker,generation,slot,backend,next_turn,
        memory && memory->header?memory->header->query_selections:0,token,diagnostic);
}
int select_and_reserve_query_locked(struct shared_backend_memory *memory,
    enum distribution_policy policy,uint32_t worker,uint64_t generation,uint32_t slot,
    struct backend_request_token *token,uint32_t *backend,struct backend_state_diagnostic *diagnostic) {
    if(require_lock(memory,diagnostic)<0)return -1;
    if(!backend){errno=EINVAL;return -1;}
    struct distribution_candidate candidates[2];
    for(size_t i=0;i<2;++i)candidates[i]=(struct distribution_candidate){
        memory->header->outstanding[i+1],memory->header->timing[i+1],true};
    struct distribution_turn turn={(uint32_t)memory->header->next_query_turn,memory->header->query_selections},next;
    size_t selected;
    if(choose_distribution_candidate(policy,candidates,2,&turn,&selected,&next)<0)return -1;
    if(reserve_request_transaction(memory,worker,generation,slot,(uint32_t)selected+1,
        next.cursor,next.selections,token,diagnostic)<0)return -1;
    *backend=(uint32_t)selected+1;return 0;
}
int complete_backend_request_locked(struct shared_backend_memory *memory,
    const struct backend_request_token *token,struct backend_state_diagnostic *diagnostic) {
    const struct distribution_observation no_sample={0};
    return complete_backend_request_observed_locked(memory,token,&no_sample,diagnostic);
}
int complete_backend_request_observed_locked(struct shared_backend_memory *memory,
                                     const struct backend_request_token *token,
                                     const struct distribution_observation *sample,
                                     struct backend_state_diagnostic *diagnostic) {
    if (require_lock(memory, diagnostic) < 0) return -1;
    if (!valid_distribution_observation(sample)) { errno=EINVAL; return -1; }
    if (!token || token->worker_index >= memory->layout.worker_capacity ||
        token->reservation_index >= memory->layout.reservation_capacity) { errno = EINVAL; return -1; }
    struct backend_request_reservation *request = &memory->reservations[token->reservation_index];
    memory->diagnostic_worker = token->worker_index;
    memory->diagnostic_generation = token->worker_generation;
    if (request->state > BACKEND_RESERVATION_ACTIVE)
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_RESERVATION);
    if (request->state == BACKEND_RESERVATION_FREE ||
        request->worker_index != token->worker_index ||
        request->worker_generation != token->worker_generation ||
        request->request_generation != token->request_generation) return 1;
    if (check_active_reservation(memory, token->reservation_index, diagnostic) < 0) return -1;
    uint32_t backend = request->backend_index;
    struct backend_worker_state *worker = &memory->workers[token->worker_index];
    prepare_change(memory, token->worker_index, token->reservation_index);
    request->state = BACKEND_RESERVATION_FREE;
    CHECKPOINT(BACKEND_FAULT_RESERVATION_WRITTEN);
    --worker->outstanding[backend];
    CHECKPOINT(BACKEND_FAULT_WORKER_WRITTEN);
    --memory->header->outstanding[backend];
    CHECKPOINT(BACKEND_FAULT_TOTAL_WRITTEN);
    struct distribution_timing timing=memory->header->timing[backend];
    update_distribution_timing(&timing,sample);
    memory->header->timing[backend].representative_us=timing.representative_us;
    CHECKPOINT(BACKEND_FAULT_TIMING_REPRESENTATIVE);
    memory->header->timing[backend].last_finished_us=timing.last_finished_us;
    CHECKPOINT(BACKEND_FAULT_TIMING_WRITTEN);
    CHECKPOINT(BACKEND_FAULT_TURN_WRITTEN);
    commit_change(memory);
    return 0;
}

int reclaim_backend_worker_locked(struct shared_backend_memory *memory,
                                   uint32_t worker_index, uint64_t generation,
                                   struct backend_state_diagnostic *diagnostic) {
    if (require_lock(memory, diagnostic) < 0) return -1;
    if (worker_index >= memory->layout.worker_capacity) { errno = EINVAL; return -1; }
    if (validate_backend_reservations_locked(memory, diagnostic) < 0) return -1;
    struct backend_worker_state *worker = &memory->workers[worker_index];
    memory->diagnostic_worker = worker_index;
    memory->diagnostic_generation = worker->generation;
    memory->recovery_stage = BACKEND_STAGE_UPDATE;
    if (worker->generation != generation || worker->state == BACKEND_WORKER_EMPTY ||
        worker->state == BACKEND_WORKER_RETIRED) return 0;
    if (worker->state == BACKEND_WORKER_ACTIVE)
        change_worker_state(memory, worker_index, BACKEND_WORKER_RETIRING, generation);
    for (uint32_t i = 0; i < memory->layout.reservation_capacity; ++i) {
        const struct backend_request_reservation *request = &memory->reservations[i];
        memory->diagnostic_worker = request->worker_index;
        memory->diagnostic_generation = request->worker_generation;
        if (request->state == BACKEND_RESERVATION_ACTIVE && request->worker_index == worker_index &&
            request->worker_generation == generation) {
            struct backend_request_token token = {worker_index, i, generation, request->request_generation};
            if (complete_backend_request_locked(memory, &token, diagnostic) != 0) return -1;
        }
    }
    change_worker_state(memory, worker_index, BACKEND_WORKER_RETIRED, generation);
    return 0;
}

int format_backend_state_diagnostic(char *buffer, size_t capacity,
                                    const struct backend_state_diagnostic *diagnostic) {
    if (!buffer || !capacity || !diagnostic) { errno = EINVAL; return -1; }
    int count = snprintf(buffer, capacity,
        "backend-state worker=%" PRIu32 " generation=%" PRIu64
        " failed_check=%u recovery_phase=%u recovery_stage=%u reason=untrusted_shared_state\n",
        diagnostic->worker_index, diagnostic->worker_generation,
        (unsigned int)diagnostic->failed_check, diagnostic->recovery_phase, diagnostic->recovery_stage);
    if (count < 0 || (size_t)count >= capacity) { errno = ENOSPC; return -1; }
    return count;
}

static int require_lock(struct shared_backend_memory *memory,
                         struct backend_state_diagnostic *diagnostic) {
    if (!memory || !memory->header || !memory->lock_held) { errno = EPERM; return -1; }
    if (check_header(memory, diagnostic) < 0) return -1;
    memory->recovery_stage = BACKEND_STAGE_UPDATE;
    return 0;
}

static int check_header(struct shared_backend_memory *memory,
                         struct backend_state_diagnostic *diagnostic) {
    memory->recovery_stage = BACKEND_STAGE_HEADER;
    struct shared_backend_header *header = memory->header;
    if (atomic_load_explicit(&header->fatal, memory_order_seq_cst))
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_FATAL);
    if (header->magic != SHARED_BACKEND_MAGIC || header->version != SHARED_BACKEND_VERSION ||
        header->worker_capacity != memory->layout.worker_capacity ||
        header->reservation_capacity != memory->layout.reservation_capacity ||
        header->next_query_turn > 1 || header->query_selections>=DISTRIBUTION_EXPLORE_INTERVAL || header->policy_reserved)
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_HEADER);
    for (size_t b = 0; b < SHARED_BACKEND_COUNT; ++b)
        if (header->outstanding[b] > memory->layout.reservation_capacity)
            return reject_corruption(memory, diagnostic, BACKEND_CHECK_TOTAL);
    return 0;
}

static int reject_corruption(struct shared_backend_memory *memory,
                              struct backend_state_diagnostic *diagnostic,
                              enum backend_state_check check) {
    atomic_store_explicit(&memory->header->fatal, 1, memory_order_seq_cst);
    if (diagnostic) {
        const struct backend_recovery_record *record = &memory->header->recovery;
        *diagnostic = (struct backend_state_diagnostic){
            .worker_index = memory->diagnostic_worker,
            .worker_generation = memory->diagnostic_generation,
            .failed_check = check,
            .recovery_phase = atomic_load_explicit(&record->phase, memory_order_seq_cst),
            .recovery_stage = memory->recovery_stage
        };
    }
    errno = EUCLEAN;
    return -1;
}

static int check_active_reservation(struct shared_backend_memory *memory,
                                     uint32_t index,
                                     struct backend_state_diagnostic *diagnostic) {
    const struct backend_request_reservation *request = &memory->reservations[index];
    if (request->worker_index >= memory->layout.worker_capacity ||
        request->backend_index >= SHARED_BACKEND_COUNT || !request->request_generation ||
        !request->worker_generation || request->reserved)
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_RESERVATION);
    const struct backend_worker_state *worker = &memory->workers[request->worker_index];
    if ((worker->state != BACKEND_WORKER_ACTIVE && worker->state != BACKEND_WORKER_RETIRING) ||
        worker->generation != request->worker_generation)
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_WORKER);
    uint64_t contribution = worker->outstanding[request->backend_index];
    if (!contribution || contribution > memory->header->outstanding[request->backend_index])
        return reject_corruption(memory, diagnostic, BACKEND_CHECK_CONTRIBUTION);
    return 0;
}

static void prepare_change(struct shared_backend_memory *memory,
                            uint32_t worker, uint32_t reservation) {
    struct backend_recovery_record *record = &memory->header->recovery;
    record->worker_index = worker;
    record->reservation_index = reservation;
    record->reserved = 0;
    record->previous_query_turn = memory->header->next_query_turn;
    record->previous_query_selections = memory->header->query_selections;
    record->policy_reserved = 0;
    memcpy(record->previous_timing,memory->header->timing,sizeof(record->previous_timing));
    memcpy(record->previous_outstanding, memory->header->outstanding, sizeof(record->previous_outstanding));
    record->previous_worker = memory->workers[worker];
    record->previous_reservation = reservation == UINT32_MAX ?
        (struct backend_request_reservation){0} : memory->reservations[reservation];
    record->checksum = recovery_checksum(record);
    CHECKPOINT(BACKEND_FAULT_JOURNAL_FILLED);
    atomic_store_explicit(&record->phase, BACKEND_TX_PREPARED, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    CHECKPOINT(BACKEND_FAULT_PREPARED);
}

static void commit_change(struct shared_backend_memory *memory) {
    atomic_thread_fence(memory_order_seq_cst);
    atomic_store_explicit(&memory->header->recovery.phase, BACKEND_TX_COMMITTED, memory_order_seq_cst);
    CHECKPOINT(BACKEND_FAULT_COMMITTED);
    atomic_store_explicit(&memory->header->recovery.phase, BACKEND_TX_IDLE, memory_order_seq_cst);
    CHECKPOINT(BACKEND_FAULT_IDLE);
}

static void change_worker_state(struct shared_backend_memory *memory,
                                uint32_t worker, uint32_t state,
                                uint64_t generation) {
    prepare_change(memory, worker, UINT32_MAX);
    memory->workers[worker].generation = generation;
    CHECKPOINT(BACKEND_FAULT_WORKER_GENERATION_WRITTEN);
    memory->workers[worker].state = state;
    CHECKPOINT(BACKEND_FAULT_WORKER_WRITTEN);
    commit_change(memory);
}

static uint64_t hash_number(uint64_t hash, uint64_t number) {
    for (unsigned int i = 0; i < 8; ++i) {
        hash ^= number & UINT64_C(255);
        hash *= UINT64_C(1099511628211);
        number >>= 8;
    }
    return hash;
}

static uint64_t recovery_checksum(const struct backend_recovery_record *record) {
    uint64_t hash = UINT64_C(14695981039346656037);
#define HASH(value) hash = hash_number(hash, (uint64_t)(value))
    HASH(record->worker_index); HASH(record->reservation_index); HASH(record->reserved);
    HASH(record->previous_query_turn); HASH(record->previous_query_selections); HASH(record->policy_reserved);
    for(size_t i=0;i<SHARED_BACKEND_COUNT;++i){
        HASH(record->previous_timing[i].representative_us);HASH(record->previous_timing[i].last_finished_us);
    }
    for (size_t b = 0; b < SHARED_BACKEND_COUNT; ++b) {
        HASH(record->previous_outstanding[b]); HASH(record->previous_worker.outstanding[b]);
    }
    HASH(record->previous_worker.generation); HASH(record->previous_worker.state);
    HASH(record->previous_worker.reserved);
    HASH(record->previous_reservation.request_generation);
    HASH(record->previous_reservation.worker_generation);
    HASH(record->previous_reservation.worker_index); HASH(record->previous_reservation.backend_index);
    HASH(record->previous_reservation.state); HASH(record->previous_reservation.reserved);
#undef HASH
    return hash;
}

static int recover_interrupted_change(struct shared_backend_memory *memory,
                                      struct backend_state_diagnostic *diagnostic) {
    struct backend_recovery_record *record = &memory->header->recovery;
    memory->recovery_stage = BACKEND_STAGE_JOURNAL;
    memory->diagnostic_worker = record->worker_index;
    memory->diagnostic_generation = record->previous_worker.generation;
    unsigned int phase = atomic_load_explicit(&record->phase, memory_order_seq_cst);
    if (phase != BACKEND_TX_IDLE) {
        if ((phase != BACKEND_TX_PREPARED && phase != BACKEND_TX_COMMITTED) ||
            record->checksum != recovery_checksum(record) || record->reserved ||
            record->worker_index >= memory->layout.worker_capacity ||
            (record->reservation_index != UINT32_MAX &&
             record->reservation_index >= memory->layout.reservation_capacity) ||
            record->previous_query_turn > 1 || record->policy_reserved ||
            record->previous_query_selections >= DISTRIBUTION_EXPLORE_INTERVAL)
            return reject_corruption(memory, diagnostic, BACKEND_CHECK_JOURNAL);
    }
    if (phase == BACKEND_TX_PREPARED) {
        memory->recovery_stage = BACKEND_STAGE_RESTORE_RESERVATION;
        if (record->reservation_index != UINT32_MAX)
            memory->reservations[record->reservation_index] = record->previous_reservation;
        CHECKPOINT(BACKEND_FAULT_RESTORE_RESERVATION);
        memory->recovery_stage = BACKEND_STAGE_RESTORE_WORKER;
        memory->workers[record->worker_index] = record->previous_worker;
        CHECKPOINT(BACKEND_FAULT_RESTORE_WORKER);
        memory->recovery_stage = BACKEND_STAGE_RESTORE_TOTAL;
        memcpy(memory->header->outstanding, record->previous_outstanding, sizeof(record->previous_outstanding));
        CHECKPOINT(BACKEND_FAULT_RESTORE_TOTAL);
        memory->recovery_stage = BACKEND_STAGE_RESTORE_TURN;
        memory->header->next_query_turn = record->previous_query_turn;
        memory->header->query_selections = record->previous_query_selections;
        for(size_t i=0;i<SHARED_BACKEND_COUNT;++i){
            memory->header->timing[i].representative_us=record->previous_timing[i].representative_us;
            CHECKPOINT(BACKEND_FAULT_RESTORE_TIMING_REPRESENTATIVE);
            memory->header->timing[i].last_finished_us=record->previous_timing[i].last_finished_us;
        }
        CHECKPOINT(BACKEND_FAULT_RESTORE_TIMING);
        CHECKPOINT(BACKEND_FAULT_RESTORE_TURN);
    }
    if (validate_backend_reservations_locked(memory, diagnostic) < 0) return -1;
    CHECKPOINT(BACKEND_FAULT_RECOVERY_VALIDATED);
    atomic_store_explicit(&record->phase, BACKEND_TX_IDLE, memory_order_seq_cst);
    CHECKPOINT(BACKEND_FAULT_RECOVERY_IDLE);
    return 0;
}
