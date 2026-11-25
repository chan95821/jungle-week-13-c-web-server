#include "backend_request_lifetime.h"
#include <errno.h>

static int invalid(int error) { errno=error;return -1; }
int begin_backend_request_locked(struct shared_backend_memory *memory,
    struct backend_request_lifetime *lifetime,uint32_t worker,uint64_t generation,
    uint32_t slot,uint32_t backend,uint64_t next_turn,
    struct backend_state_diagnostic *diagnostic) {
    if(!lifetime) return invalid(EINVAL);
    if(lifetime->phase!=BACKEND_REQUEST_IDLE) return invalid(EBUSY);
    struct backend_request_token token;
    if(reserve_backend_request_locked(memory,worker,generation,slot,backend,next_turn,&token,diagnostic)<0)
        return -1;
    *lifetime=(struct backend_request_lifetime){.token=token,.phase=BACKEND_REQUEST_ACTIVE};
    return 0;
}
int begin_selected_query_request_locked(struct shared_backend_memory *memory,
    struct backend_request_lifetime *lifetime,enum distribution_policy policy,
    uint32_t worker,uint64_t generation,uint32_t slot,uint32_t *backend,
    struct backend_state_diagnostic *diagnostic) {
    if(!lifetime)return invalid(EINVAL);
    if(lifetime->phase!=BACKEND_REQUEST_IDLE)return invalid(EBUSY);
    struct backend_request_token token;
    if(select_and_reserve_query_locked(memory,policy,worker,generation,slot,&token,backend,diagnostic)<0)return -1;
    *lifetime=(struct backend_request_lifetime){.token=token,.phase=BACKEND_REQUEST_ACTIVE};return 0;
}
int start_backend_request_clock(struct backend_request_lifetime *lifetime,const struct timespec *now) {
    if(!lifetime || lifetime->phase!=BACKEND_REQUEST_ACTIVE || lifetime->clock_started || !now)
        return invalid(EINVAL);
    struct distribution_observation check;
    if(distribution_elapsed_us(now,now,&check,DISTRIBUTION_COMPLETED)<0)return -1;
    lifetime->started_at=*now;lifetime->clock_started=true;return 0;
}
int finish_backend_request_clock(struct backend_request_lifetime *lifetime,
    const struct timespec *now,enum backend_request_outcome outcome) {
    if(!lifetime || outcome<BACKEND_OUTCOME_COMPLETE || outcome>BACKEND_OUTCOME_CANCELLED)return invalid(EINVAL);
    if(lifetime->phase==BACKEND_REQUEST_RELEASE_PENDING || lifetime->phase==BACKEND_REQUEST_RELEASED)return 1;
    if(lifetime->phase!=BACKEND_REQUEST_ACTIVE || !lifetime->clock_started)return invalid(EINVAL);
    enum distribution_sample_result result=outcome==BACKEND_OUTCOME_COMPLETE?DISTRIBUTION_COMPLETED:
        outcome==BACKEND_OUTCOME_FAILED?DISTRIBUTION_FAILED:DISTRIBUTION_CANCELLED;
    struct distribution_observation sample;
    int clock_result=distribution_elapsed_us(&lifetime->started_at,now,&sample,result);
    int clock_error=errno;
    /* Even invalid timing must not prevent reservation release; report error,
     * preserve first outcome, omit invalid sample. Caller logs numeric error. */
    int marked=mark_backend_request_finished(lifetime,outcome);
    if(clock_result<0){errno=clock_error;return -1;}
    lifetime->observation=sample;return marked;
}
int mark_backend_request_finished(struct backend_request_lifetime *lifetime,
                                  enum backend_request_outcome outcome) {
    if(!lifetime || outcome<BACKEND_OUTCOME_COMPLETE || outcome>BACKEND_OUTCOME_CANCELLED)
        return invalid(EINVAL);
    if(lifetime->phase==BACKEND_REQUEST_RELEASE_PENDING || lifetime->phase==BACKEND_REQUEST_RELEASED)
        return 1;
    if(lifetime->phase!=BACKEND_REQUEST_ACTIVE) return invalid(EINVAL);
    lifetime->outcome=outcome;lifetime->phase=BACKEND_REQUEST_RELEASE_PENDING;
    return 0;
}
int try_release_backend_request(struct shared_backend_memory *memory,
    struct backend_request_lifetime *lifetime,struct backend_state_diagnostic *diagnostic) {
    if(!lifetime || !memory || !memory->header) return invalid(EINVAL);
    if(lifetime->phase==BACKEND_REQUEST_RELEASED) return 0;
    if(lifetime->phase!=BACKEND_REQUEST_RELEASE_PENDING) return invalid(EINVAL);
    if(try_lock_shared_backend_state(memory,diagnostic)<0) return errno==EBUSY?1:-1;
    int result=complete_backend_request_observed_locked(memory,&lifetime->token,&lifetime->observation,diagnostic);
    int error=errno;
    if(unlock_shared_backend_state(memory)<0) return -1;
    if(result<0) return invalid(error);
    /* Stale/already released is also safe: the generation-aware ledger never
     * decrements a reused reservation. No network/file I/O occurs under lock. */
    lifetime->phase=BACKEND_REQUEST_RELEASED;
    return 0;
}
int reset_backend_request_lifetime(struct backend_request_lifetime *lifetime) {
    if(!lifetime) return invalid(EINVAL);
    if(lifetime->phase==BACKEND_REQUEST_ACTIVE || lifetime->phase==BACKEND_REQUEST_RELEASE_PENDING)
        return invalid(EBUSY);
    if(lifetime->phase!=BACKEND_REQUEST_IDLE && lifetime->phase!=BACKEND_REQUEST_RELEASED)
        return invalid(EINVAL);
    *lifetime=(struct backend_request_lifetime){0};return 0;
}
