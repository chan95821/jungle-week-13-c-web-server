#include "parent_connection_ledger.h"
#include <errno.h>
#include <stdlib.h>

static bool valid_owner(const struct parent_connection_ledger *ledger) {
    return ledger && ledger->entries && ledger->workers && ledger->capacity &&
           ledger->capacity <= UINT32_MAX && ledger->worker_capacity && ledger->worker_limit;
}
static int invalid(int error) { errno = error; return -1; }
/* 0 current, 1 older, -1 invalid/future; output unchanged on failure. */
static int find_connection(struct parent_connection_ledger *ledger, uint64_t id,
                            struct parent_connection_entry **entry) {
    if (!valid_owner(ledger) || !id) return invalid(EINVAL);
    uint32_t index = (uint32_t)id;
    if (!index || index > ledger->capacity || !(id >> 32)) return invalid(EPROTO);
    struct parent_connection_entry *found = &ledger->entries[index - 1];
    if (id > found->connection_id) return invalid(EPROTO);
    if (id < found->connection_id) return 1;
    *entry = found;
    return 0;
}
static void release_connection(struct parent_connection_ledger *ledger,
                                struct parent_connection_entry *entry) {
    if (entry->phase == PARENT_CONNECTION_QUEUED) --ledger->queued;
    else {
        --ledger->workers[entry->worker_index].outstanding;
        update_distribution_timing(&ledger->workers[entry->worker_index].timing,&entry->completion);
    }
    --ledger->total;
    entry->phase = PARENT_CONNECTION_FREE;
    entry->closed_before_handoff = false;
    entry->completion=(struct distribution_observation){0};
}
int create_parent_connection_ledger(size_t capacity, size_t workers, size_t worker_limit,
                                    struct parent_connection_ledger *ledger) {
    if (!ledger || !capacity || !workers || !worker_limit) return invalid(EINVAL);
    if (ledger->entries || ledger->workers) return invalid(EBUSY);
    if (capacity > UINT32_MAX || workers > UINT32_MAX ||
        capacity > SIZE_MAX / sizeof(*ledger->entries) ||
        workers > SIZE_MAX / sizeof(*ledger->workers)) return invalid(EOVERFLOW);
    struct parent_connection_entry *entries = calloc(capacity, sizeof(*entries));
    if (!entries) return -1;
    struct parent_connection_worker *states = calloc(workers, sizeof(*states));
    if (!states) { int error = errno; free(entries); return invalid(error); }
    struct distribution_candidate *candidates=calloc(workers,sizeof(*candidates));
    if(!candidates){int error=errno;free(entries);free(states);return invalid(error);}
    *ledger = (struct parent_connection_ledger){.entries=entries, .workers=states,.selection_candidates=candidates,
        .capacity=capacity, .worker_capacity=workers, .worker_limit=worker_limit};
    return 0;
}
void destroy_parent_connection_ledger(struct parent_connection_ledger *ledger) {
    if (!ledger) return;
    free(ledger->entries); free(ledger->workers);free(ledger->selection_candidates);
    *ledger = (struct parent_connection_ledger){0};
}
int activate_parent_connection_worker(struct parent_connection_ledger *ledger,
                                      uint32_t worker, uint64_t generation) {
    if (!valid_owner(ledger) || worker >= ledger->worker_capacity || !generation) return invalid(EINVAL);
    struct parent_connection_worker *state = &ledger->workers[worker];
    if (state->active || state->outstanding) return invalid(EBUSY);
    if (generation <= state->generation) return invalid(EPROTO);
    *state = (struct parent_connection_worker){.generation=generation, .active=true};
    return 0;
}
int register_parent_connection(struct parent_connection_ledger *ledger, uint64_t *id) {
    if (!valid_owner(ledger) || !id) return invalid(EINVAL);
    if (ledger->total >= ledger->capacity) return invalid(ENOSPC);
    for (size_t i=0; i<ledger->capacity; ++i) {
        struct parent_connection_entry *entry = &ledger->entries[i];
        if (entry->phase != PARENT_CONNECTION_FREE) continue;
        uint64_t generation = entry->connection_id >> 32;
        if (generation == UINT32_MAX) return invalid(EOVERFLOW);
        *entry = (struct parent_connection_entry){
            .connection_id=((generation+1)<<32)|(i+1), .phase=PARENT_CONNECTION_QUEUED};
        ++ledger->total; ++ledger->queued; *id = entry->connection_id;
        return 0;
    }
    return invalid(EPROTO);
}
int assign_parent_connection(struct parent_connection_ledger *ledger, uint64_t id,
                              uint32_t worker, uint64_t generation) {
    struct parent_connection_entry *entry;
    int result=find_connection(ledger,id,&entry);
    if (result) return result < 0 ? -1 : invalid(EPROTO);
    if (worker >= ledger->worker_capacity || entry->phase != PARENT_CONNECTION_QUEUED)
        return invalid(EPROTO);
    struct parent_connection_worker *state=&ledger->workers[worker];
    if (!state->active || generation != state->generation) return invalid(EPROTO);
    if (state->outstanding >= ledger->worker_limit) return invalid(EAGAIN);
    entry->worker_index=worker; entry->worker_generation=generation;
    entry->phase=PARENT_CONNECTION_SENDING;
    --ledger->queued; ++state->outstanding;
    return 0;
}
int select_and_assign_parent_connection(struct parent_connection_ledger *ledger,
    uint64_t id,enum distribution_policy policy,const bool *available,uint32_t *worker) {
    if(!valid_owner(ledger) || !available || !worker || !ledger->selection_candidates)return invalid(EINVAL);
    for(size_t i=0;i<ledger->worker_capacity;++i)ledger->selection_candidates[i]=(struct distribution_candidate){
        ledger->workers[i].outstanding,ledger->workers[i].timing,
        available[i] && ledger->workers[i].active && ledger->workers[i].outstanding<ledger->worker_limit};
    size_t selected;struct distribution_turn next;
    if(choose_distribution_candidate(policy,ledger->selection_candidates,ledger->worker_capacity,
        &ledger->selection_turn,&selected,&next)<0)return -1;
    if(assign_parent_connection(ledger,id,(uint32_t)selected,ledger->workers[selected].generation)<0)return -1;
    ledger->selection_turn=next;*worker=(uint32_t)selected;return 0;
}
int finish_parent_connection_handoff(struct parent_connection_ledger *ledger, uint64_t id) {
    struct parent_connection_entry *entry;
    int result=find_connection(ledger,id,&entry);
    if (result) return result < 0 ? -1 : invalid(EPROTO);
    if (entry->phase != PARENT_CONNECTION_SENDING) return invalid(EPROTO);
    if (entry->closed_before_handoff) release_connection(ledger,entry);
    else entry->phase=PARENT_CONNECTION_OWNED;
    return 0;
}
int cancel_untransferred_parent_connection(struct parent_connection_ledger *ledger,uint64_t id) {
    struct parent_connection_entry *entry;
    int result=find_connection(ledger,id,&entry);
    if(result)return result<0?-1:invalid(EPROTO);
    if(entry->phase!=PARENT_CONNECTION_SENDING || entry->closed_before_handoff)return invalid(EPROTO);
    release_connection(ledger,entry);return 0;
}
int cancel_queued_parent_connection(struct parent_connection_ledger *ledger, uint64_t id) {
    struct parent_connection_entry *entry;
    int result=find_connection(ledger,id,&entry);
    if (result) return result < 0 ? -1 : invalid(EPROTO);
    if (entry->phase != PARENT_CONNECTION_QUEUED) return invalid(EPROTO);
    release_connection(ledger,entry);
    return 0;
}
int apply_worker_connection_closed(struct parent_connection_ledger *ledger,
    uint32_t worker,uint64_t generation,uint64_t id) {
    const struct distribution_observation none={0};
    return apply_worker_connection_observed_closed(ledger,worker,generation,id,&none);
}
int apply_worker_connection_observed_closed(struct parent_connection_ledger *ledger,
    uint32_t worker,uint64_t generation,uint64_t id,const struct distribution_observation *sample) {
    if(!valid_distribution_observation(sample))return invalid(EINVAL);
    if (!valid_owner(ledger) || worker >= ledger->worker_capacity || !generation) return invalid(EINVAL);
    if (generation > ledger->workers[worker].generation) return invalid(EPROTO);
    if (generation < ledger->workers[worker].generation) return 1;
    struct parent_connection_entry *entry;
    int result=find_connection(ledger,id,&entry);
    if (result) return result;
    if (entry->phase == PARENT_CONNECTION_QUEUED || entry->worker_index != worker ||
        entry->worker_generation != generation) return invalid(EPROTO);
    if (entry->phase == PARENT_CONNECTION_FREE || entry->closed_before_handoff) return 1;
    entry->completion=*sample;
    if (entry->phase == PARENT_CONNECTION_SENDING) entry->closed_before_handoff=true;
    else release_connection(ledger,entry);
    return 0;
}
int reclaim_reaped_worker_connections(struct parent_connection_ledger *ledger,
                                       uint32_t worker, uint64_t generation) {
    if (!valid_owner(ledger) || worker >= ledger->worker_capacity || !generation) return invalid(EINVAL);
    struct parent_connection_worker *state=&ledger->workers[worker];
    if (generation > state->generation) return invalid(EPROTO);
    if (generation < state->generation || !state->active) return 1;
    for (size_t i=0; i<ledger->capacity; ++i) {
        struct parent_connection_entry *entry=&ledger->entries[i];
        if ((entry->phase == PARENT_CONNECTION_SENDING || entry->phase == PARENT_CONNECTION_OWNED) &&
            entry->worker_index == worker && entry->worker_generation == generation)
            release_connection(ledger,entry);
    }
    state->active=false;
    return 0;
}
int validate_parent_connection_ledger(const struct parent_connection_ledger *ledger) {
    if (!valid_owner(ledger)) return invalid(EINVAL);
    size_t total=0,queued=0;
    for (size_t i=0; i<ledger->capacity; ++i) {
        const struct parent_connection_entry *entry=&ledger->entries[i];
        if (entry->phase < PARENT_CONNECTION_FREE || entry->phase > PARENT_CONNECTION_OWNED)
            return invalid(EPROTO);
        if (entry->connection_id && ((uint32_t)entry->connection_id != i+1 || !(entry->connection_id>>32)))
            return invalid(EPROTO);
        if (entry->phase == PARENT_CONNECTION_FREE) {
            if (entry->closed_before_handoff) return invalid(EPROTO);
            continue;
        }
        if (!entry->connection_id) return invalid(EPROTO);
        ++total;
        if (entry->phase == PARENT_CONNECTION_QUEUED) ++queued;
        else if (entry->worker_index >= ledger->worker_capacity ||
                 !ledger->workers[entry->worker_index].active ||
                 entry->worker_generation != ledger->workers[entry->worker_index].generation)
            return invalid(EPROTO);
        if (entry->closed_before_handoff && entry->phase != PARENT_CONNECTION_SENDING) return invalid(EPROTO);
    }
    if (total != ledger->total || queued != ledger->queued) return invalid(EPROTO);
    for (size_t worker=0; worker<ledger->worker_capacity; ++worker) {
        size_t count=0;
        for (size_t i=0; i<ledger->capacity; ++i)
            if ((ledger->entries[i].phase == PARENT_CONNECTION_SENDING ||
                 ledger->entries[i].phase == PARENT_CONNECTION_OWNED) && ledger->entries[i].worker_index == worker) ++count;
        if (count != ledger->workers[worker].outstanding || count > ledger->worker_limit)
            return invalid(EPROTO);
    }
    return 0;
}
