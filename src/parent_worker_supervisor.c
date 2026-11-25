#define _POSIX_C_SOURCE 200809L
#include "parent_worker_supervisor.h"
#include "concurrency_limits.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef PARENT_WORKER_SUPERVISOR_TEST
extern pid_t supervisor_test_waitpid(pid_t pid, int *status, int options);
extern int supervisor_test_kill(pid_t pid, int signal_number);
extern int supervisor_test_backend_trylock(struct shared_backend_memory *memory,
    struct backend_state_diagnostic *diagnostic);
#define supervisor_waitpid supervisor_test_waitpid
#define supervisor_kill supervisor_test_kill
#define supervisor_backend_trylock supervisor_test_backend_trylock
#else
#define supervisor_waitpid waitpid
#define supervisor_kill kill
#define supervisor_backend_trylock try_lock_shared_backend_state
#endif
static int invalid(int error) { errno=error; return -1; }
static bool valid(const struct parent_worker_supervisor *supervisor) {
    return supervisor && supervisor->workers && supervisor->capacity && supervisor->ledger &&
           supervisor->backend && supervisor->backend->header && supervisor->queue;
}
static bool valid_time(const struct parent_worker_supervisor *supervisor,uint64_t now) {
    return now>=supervisor->last_now_ms && now<=UINT64_MAX-INITIAL_WORKER_STOP_GRACE_MS &&
           now<=UINT64_MAX-INITIAL_SUPERVISOR_RETRY_MS;
}
static bool valid_reason(enum parent_worker_stop_reason reason) {
    return reason>=PARENT_STOP_CHANNEL && reason<=PARENT_STOP_SYSTEM_ERROR;
}
static bool shared_fatal(const struct parent_worker_supervisor *supervisor) {
    return atomic_load_explicit(&supervisor->backend->header->fatal,memory_order_acquire)!=0;
}
static void record_failure(struct parent_worker_supervisor *supervisor,uint32_t worker,
                            enum parent_supervisor_failure operation,int error,
                            const struct backend_state_diagnostic *backend) {
    supervisor->exit_error=true;
    if(supervisor->diagnostic.failed_operation!=PARENT_SUPERVISOR_NONE) return;
    supervisor->diagnostic=(struct parent_supervisor_diagnostic){.worker_index=worker,
        .worker_generation=worker<supervisor->capacity?supervisor->workers[worker].generation:0,
        .failed_operation=operation,.system_error=error};
    if(backend) supervisor->diagnostic.backend=*backend;
}
int read_parent_supervisor_time_ms(uint64_t *now) {
    if(!now) return invalid(EINVAL);
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC,&time)<0) return -1;
    if(time.tv_sec<0 || (uint64_t)time.tv_sec>(UINT64_MAX-999)/1000) return invalid(EOVERFLOW);
    *now=(uint64_t)time.tv_sec*1000+(uint64_t)time.tv_nsec/1000000;
    return 0;
}
int create_parent_worker_supervisor(struct parent_connection_ledger *ledger,
    struct shared_backend_memory *backend,struct parent_client_queue *queue,
    struct parent_worker_supervisor *supervisor) {
    if(!ledger || !ledger->entries || !ledger->workers || !backend || !backend->header ||
       !queue || !queue->clients || !supervisor || !ledger->worker_capacity ||
       ledger->worker_capacity!=backend->layout.worker_capacity) return invalid(EINVAL);
    if(supervisor->workers) return invalid(EBUSY);
    if(ledger->worker_capacity>SIZE_MAX/sizeof(*supervisor->workers)) return invalid(EOVERFLOW);
    struct parent_supervised_worker *workers=calloc(ledger->worker_capacity,sizeof(*workers));
    if(!workers) return -1;
    for(size_t i=0;i<ledger->worker_capacity;++i) workers[i].channel_fd=-1;
    *supervisor=(struct parent_worker_supervisor){.workers=workers,.capacity=ledger->worker_capacity,
        .queue=queue,.ledger=ledger,.backend=backend};
    return 0;
}
int adopt_parent_worker(struct parent_worker_supervisor *supervisor,uint32_t worker,
                         uint64_t generation,pid_t pid,int *channel) {
    if(!valid(supervisor) || worker>=supervisor->capacity || !generation || pid<=0 ||
       !channel || *channel<0) return invalid(EINVAL);
    if(supervisor->started || supervisor->stopping || supervisor->workers[worker].phase!=PARENT_WORKER_UNUSED)
        return invalid(EBUSY);
    const struct parent_connection_worker *state=&supervisor->ledger->workers[worker];
    if(!state->active || state->generation!=generation) return invalid(EPROTO);
    for(size_t i=0;i<supervisor->capacity;++i)
        if(supervisor->workers[i].phase!=PARENT_WORKER_UNUSED && supervisor->workers[i].pid==pid)
            return invalid(EINVAL);
    /* Shared generation is caller-validated before fork under its lock; do not
     * read mutable shared worker fields without that lock here. */
    supervisor->workers[worker]=(struct parent_supervised_worker){.pid=pid,.generation=generation,
        .channel_fd=*channel,.phase=PARENT_WORKER_RUNNING};
    *channel=-1;return 0;
}
int finish_parent_worker_startup(struct parent_worker_supervisor *supervisor) {
    if(!valid(supervisor) || supervisor->started || supervisor->stopping) return invalid(EINVAL);
    for(size_t i=0;i<supervisor->capacity;++i)
        if(supervisor->workers[i].phase!=PARENT_WORKER_RUNNING) return invalid(EINVAL);
    supervisor->started=true;return 0;
}
bool parent_worker_accepts_assignment(const struct parent_worker_supervisor *supervisor,uint32_t worker) {
    return valid(supervisor) && supervisor->started && !supervisor->stopping && !shared_fatal(supervisor) &&
           worker<supervisor->capacity && supervisor->workers[worker].phase==PARENT_WORKER_RUNNING;
}
bool parent_supervisor_accepts_clients(const struct parent_worker_supervisor *supervisor) {
    if(!valid(supervisor)) return false;
    for(uint32_t worker=0;worker<supervisor->capacity;++worker)
        if(parent_worker_accepts_assignment(supervisor,worker)) return true;
    return false;
}
int request_parent_worker_stop(struct parent_worker_supervisor *supervisor,uint32_t worker,
    uint64_t now,enum parent_worker_stop_reason reason) {
    if(!valid(supervisor) || worker>=supervisor->capacity || !valid_time(supervisor,now) || !valid_reason(reason))
        return invalid(EINVAL);
    struct parent_supervised_worker *state=&supervisor->workers[worker];
    supervisor->last_now_ms=now;
    if(state->phase==PARENT_WORKER_RUNNING) {
        state->phase=PARENT_WORKER_STOPPING;state->stop_reason=reason;
        state->stop_deadline_ms=now+INITIAL_WORKER_STOP_GRACE_MS;
    }
    return 0;
}
int request_parent_supervisor_shutdown(struct parent_worker_supervisor *supervisor,uint64_t now,
    enum parent_worker_stop_reason reason,bool error_exit) {
    if(!valid(supervisor) || !valid_time(supervisor,now) || !valid_reason(reason)) return invalid(EINVAL);
    supervisor->last_now_ms=now;supervisor->stopping=true;supervisor->exit_error|=error_exit;
    for(uint32_t worker=0;worker<supervisor->capacity;++worker)
        request_parent_worker_stop(supervisor,worker,now,reason);
    return 0;
}
static void clear_worker_retry(struct parent_supervised_worker *state) {
    state->retry_kind=PARENT_RETRY_NONE;state->retry_at_ms=0;
}
static void schedule_worker_retry(struct parent_worker_supervisor *supervisor,
    struct parent_supervised_worker *state,enum parent_worker_retry_kind kind,uint64_t now) {
    state->retry_kind=kind;state->retry_at_ms=now+INITIAL_SUPERVISOR_RETRY_MS;
    supervisor->cleanup_pending=true;
}
static void reap_worker(struct parent_worker_supervisor *supervisor,uint32_t worker,uint64_t now) {
    struct parent_supervised_worker *state=&supervisor->workers[worker];
    if(state->phase!=PARENT_WORKER_RUNNING && state->phase!=PARENT_WORKER_STOPPING) return;
    if(state->retry_kind==PARENT_RETRY_WAIT && now<state->retry_at_ms) {
        supervisor->cleanup_pending=true;return;
    }
    clear_worker_retry(state);
    int status;pid_t result=supervisor_waitpid(state->pid,&status,WNOHANG);
    if(result<0 && errno==EINTR) {
        schedule_worker_retry(supervisor,state,PARENT_RETRY_WAIT,now);return;
    }
    if(!result) return;
    if(result==state->pid) {
        state->reaped=true;state->wait_status=status;state->phase=PARENT_WORKER_REAPED;return;
    }
    /* Do not signal a PID after losing wait ownership: it might be reused.
     * Keep reaped=false; borrowed memory must not be explicitly destroyed. */
    record_failure(supervisor,worker,PARENT_SUPERVISOR_WAIT,result<0?errno:EPROTO,NULL);
    state->phase=PARENT_WORKER_LOST;
}
static void cleanup_reaped_worker(struct parent_worker_supervisor *supervisor,uint32_t worker,uint64_t now) {
    struct parent_supervised_worker *state=&supervisor->workers[worker];
    if(state->phase!=PARENT_WORKER_REAPED) return;
    if(!shared_fatal(supervisor) && state->retry_kind==PARENT_RETRY_BACKEND && now<state->retry_at_ms) {
        supervisor->cleanup_pending=true;return;
    }
    clear_worker_retry(state);
    abort_worker_fd_send(&state->sender);
    if(state->channel_fd>=0) {
        int channel=state->channel_fd;state->channel_fd=-1;
        if(close(channel)<0) record_failure(supervisor,worker,PARENT_SUPERVISOR_CLOSE,errno,NULL);
    }
    if(reclaim_reaped_worker_connections(supervisor->ledger,worker,state->generation)<0) {
        record_failure(supervisor,worker,PARENT_SUPERVISOR_LEDGER,errno,NULL);state->cleanup_failed=true;
    }
    if(shared_fatal(supervisor)) {state->cleanup_failed=true;state->phase=PARENT_WORKER_FINISHED;return;}
    struct backend_state_diagnostic diagnostic;
    if(supervisor_backend_trylock(supervisor->backend,&diagnostic)<0) {
        if(errno==EBUSY) {schedule_worker_retry(supervisor,state,PARENT_RETRY_BACKEND,now);return;}
        record_failure(supervisor,worker,PARENT_SUPERVISOR_BACKEND,errno,&diagnostic);
        state->cleanup_failed=true;state->phase=PARENT_WORKER_FINISHED;return;
    }
    int result=reclaim_backend_worker_locked(supervisor->backend,worker,state->generation,&diagnostic);
    int error=errno;
    if(unlock_shared_backend_state(supervisor->backend)<0) {result=-1;error=errno;}
    if(result<0) {record_failure(supervisor,worker,PARENT_SUPERVISOR_BACKEND,error,&diagnostic);state->cleanup_failed=true;}
    state->phase=PARENT_WORKER_FINISHED;
}
static void discard_queued_clients(struct parent_worker_supervisor *supervisor) {
    while(supervisor->queue->count) {
        struct parent_queued_client client={.client_fd=-1};
        if(dequeue_parent_client(supervisor->queue,&client)!=1) {
            record_failure(supervisor,UINT32_MAX,PARENT_SUPERVISOR_LEDGER,errno,NULL);break;
        }
        if(close(client.client_fd)<0) record_failure(supervisor,UINT32_MAX,PARENT_SUPERVISOR_CLOSE,errno,NULL);
        if(cancel_queued_parent_connection(supervisor->ledger,client.connection_id)<0)
            record_failure(supervisor,UINT32_MAX,PARENT_SUPERVISOR_LEDGER,errno,NULL);
    }
}
static void signal_stopping_worker(struct parent_worker_supervisor *supervisor,uint32_t worker,uint64_t now) {
    struct parent_supervised_worker *state=&supervisor->workers[worker];
    if(state->phase!=PARENT_WORKER_STOPPING) return;
    if(!state->term_attempted) {
        state->term_attempted=true;
        if(supervisor_kill(state->pid,SIGTERM)<0 && errno!=ESRCH)
            record_failure(supervisor,worker,PARENT_SUPERVISOR_SIGNAL,errno,NULL);
    }
    if(now>=state->stop_deadline_ms && !state->kill_attempted) {
        state->kill_attempted=true;
        if(supervisor_kill(state->pid,SIGKILL)<0 && errno!=ESRCH)
            record_failure(supervisor,worker,PARENT_SUPERVISOR_SIGNAL,errno,NULL);
    }
}
int advance_parent_worker_supervisor(struct parent_worker_supervisor *supervisor,uint64_t now) {
    if(!valid(supervisor) || !valid_time(supervisor,now)) return invalid(EINVAL);
    supervisor->last_now_ms=now;supervisor->cleanup_pending=false;
    if(shared_fatal(supervisor))
        request_parent_supervisor_shutdown(supervisor,now,PARENT_STOP_SHARED_FATAL,true);
    for(uint32_t worker=0;worker<supervisor->capacity;++worker) reap_worker(supervisor,worker,now);
    for(uint32_t worker=0;worker<supervisor->capacity;++worker) cleanup_reaped_worker(supervisor,worker,now);
    if(!supervisor->stopping && (shared_fatal(supervisor) || supervisor->diagnostic.failed_operation!=PARENT_SUPERVISOR_NONE))
        request_parent_supervisor_shutdown(supervisor,now,PARENT_STOP_SYSTEM_ERROR,true);
    if(supervisor->started && !supervisor->stopping && !parent_supervisor_accepts_clients(supervisor))
        request_parent_supervisor_shutdown(supervisor,now,PARENT_STOP_NO_WORKERS,true);
    if(supervisor->stopping) discard_queued_clients(supervisor);
    for(uint32_t worker=0;worker<supervisor->capacity;++worker) signal_stopping_worker(supervisor,worker,now);
    /* A signal syscall failure is discovered after the first shutdown check. */
    if(!supervisor->stopping && supervisor->diagnostic.failed_operation!=PARENT_SUPERVISOR_NONE)
        request_parent_supervisor_shutdown(supervisor,now,PARENT_STOP_SYSTEM_ERROR,true);
    return supervisor->cleanup_pending?1:0;
}
static int nearest_supervisor_timeout(const struct parent_worker_supervisor *supervisor,uint64_t now,bool include_retry) {
    if(!valid(supervisor)) return -1;
    uint64_t nearest=UINT64_MAX;
    for(size_t worker=0;worker<supervisor->capacity;++worker) {
        const struct parent_supervised_worker *state=&supervisor->workers[worker];
        if(state->phase==PARENT_WORKER_STOPPING && !state->kill_attempted && state->stop_deadline_ms<nearest)
            nearest=state->stop_deadline_ms;
        if(include_retry && state->retry_kind!=PARENT_RETRY_NONE && state->retry_at_ms<nearest)
            nearest=state->retry_at_ms;
    }
    if(nearest==UINT64_MAX) return -1;
    if(nearest<=now) return 0;
    uint64_t remaining=nearest-now;return remaining>INT_MAX?INT_MAX:(int)remaining;
}
int parent_supervisor_signal_timeout_ms(const struct parent_worker_supervisor *supervisor,uint64_t now) {
    return nearest_supervisor_timeout(supervisor,now,false);
}
int parent_supervisor_wait_timeout_ms(const struct parent_worker_supervisor *supervisor,uint64_t now) {
    return nearest_supervisor_timeout(supervisor,now,true);
}
bool parent_supervisor_all_reaped(const struct parent_worker_supervisor *supervisor) {
    if(!valid(supervisor)) return false;
    for(size_t worker=0;worker<supervisor->capacity;++worker)
        if(supervisor->workers[worker].phase!=PARENT_WORKER_UNUSED && !supervisor->workers[worker].reaped) return false;
    return true;
}
bool parent_supervisor_cleanup_finished(const struct parent_worker_supervisor *supervisor) {
    if(!parent_supervisor_all_reaped(supervisor)) return false;
    for(size_t worker=0;worker<supervisor->capacity;++worker)
        if(supervisor->workers[worker].phase!=PARENT_WORKER_UNUSED && supervisor->workers[worker].phase!=PARENT_WORKER_FINISHED)
            return false;
    return true;
}
int destroy_parent_worker_supervisor(struct parent_worker_supervisor *supervisor) {
    if(!supervisor) return invalid(EINVAL);
    if(!supervisor->workers) {*supervisor=(struct parent_worker_supervisor){0};return 0;}
    if(!parent_supervisor_cleanup_finished(supervisor)) return invalid(EBUSY);
    free(supervisor->workers);*supervisor=(struct parent_worker_supervisor){0};return 0;
}
