#define _GNU_SOURCE
#include "process_http_server.h"
#include "parent_http_loop.h"
#include "worker_http_io.h"
#include "worker_failure_channel.h"
#include "concurrency_limits.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
struct process_http_runtime {
    struct shared_backend_memory shared;
    struct parent_connection_ledger ledger;
    struct parent_client_queue queue;
    struct parent_worker_supervisor supervisor;
    struct parent_http_loop loop;
    int failure[2];
    bool has_shared,has_ledger,has_queue,has_supervisor,has_loop;
};
static int run_network_worker(const struct proxy_config *config,struct process_http_runtime *runtime,
    uint32_t index,uint64_t generation,int channel,struct process_signal_wait *signals){
    struct worker_http_reactor reactor={0};
    if(create_worker_http_reactor(config,&runtime->shared,index,generation,INITIAL_WORKER_CONNECTION_LIMIT,channel,&reactor)<0)return 1;
    int result;while((result=advance_worker_http_reactor(&reactor,signals,-1))==0){}
    if(atomic_load(&runtime->shared.header->fatal)){
        publish_worker_failure(&runtime->shared,index,generation,&reactor.diagnostic);
        notify_parent_worker_failure(runtime->failure[1]);
    }
    destroy_worker_http_reactor(&reactor);return result<0?1:0;
}
static int start_network_workers(struct process_http_runtime *runtime,int listener,
    const struct proxy_config *config,struct process_signal_wait *signals){
    uint64_t generations[INITIAL_WORKER_COUNT];
    if(try_lock_shared_backend_state(&runtime->shared,NULL)<0)return -1;
    int result=0;
    for(uint32_t i=0;i<INITIAL_WORKER_COUNT;++i)
        if(activate_backend_worker_locked(&runtime->shared,i,&generations[i],NULL)<0){result=-1;break;}
    if(unlock_shared_backend_state(&runtime->shared)<0 || result<0)return -1;
    for(uint32_t i=0;i<INITIAL_WORKER_COUNT;++i){
        if(activate_parent_connection_worker(&runtime->ledger,i,generations[i])<0 ||
           arm_worker_failure_slot(&runtime->shared,i,generations[i])<0)return -1;
        int channel[2];if(create_worker_fd_channel(channel)<0)return -1;
        pid_t child=fork();if(child<0){int saved=errno;close(channel[0]);close(channel[1]);errno=saved;return -1;}
        if(!child){
            close(listener);close(runtime->loop.epoll_fd);close(runtime->failure[0]);close(channel[0]);
            for(size_t prior=0;prior<i;++prior)close(runtime->supervisor.workers[prior].channel_fd);
            *signals->state=PROCESS_SIGNAL_RUNNING;
            int status=run_network_worker(config,runtime,i,generations[i],channel[1],signals);
            close(channel[1]);close(runtime->failure[1]);_exit(status);
        }
        close(channel[1]);
        if(adopt_parent_worker(&runtime->supervisor,i,generations[i],child,&channel[0])<0){
            /* Failed adoption owns exactly this new PID; startup only. */
            int saved=errno;close(channel[0]);kill(child,SIGKILL);pid_t reaped;
            do{reaped=waitpid(child,NULL,0);}while(reaped<0 && errno==EINTR);
            errno=saved;return -1;
        }
    }
    return finish_parent_worker_startup(&runtime->supervisor);
}
static int prepare_runtime(struct process_http_runtime *runtime,int listener,
    const struct proxy_config *config,struct process_signal_wait *signals){
    if(create_shared_backend_memory(INITIAL_WORKER_COUNT,INITIAL_BACKEND_RESERVATION_CAPACITY,
        INITIAL_SHARED_MEMORY_BUDGET,&runtime->shared)<0)return -1;
    runtime->has_shared=true;
    if(create_parent_connection_ledger(INITIAL_TOTAL_CONNECTION_LIMIT,INITIAL_WORKER_COUNT,
        INITIAL_WORKER_CONNECTION_LIMIT,&runtime->ledger)<0)return -1;
    runtime->has_ledger=true;
    if(create_parent_client_queue(INITIAL_PARENT_QUEUE_CAPACITY,&runtime->queue)<0)return -1;
    runtime->has_queue=true;
    if(create_parent_worker_supervisor(&runtime->ledger,&runtime->shared,&runtime->queue,&runtime->supervisor)<0)return -1;
    runtime->has_supervisor=true;
    if(create_worker_failure_pipe(runtime->failure)<0)return -1;
    if(create_parent_http_loop(listener,runtime->failure[0],&runtime->supervisor,config,signals,&runtime->loop)<0)return -1;
    runtime->has_loop=true;return 0;
}
static int cleanup_runtime(struct process_http_runtime *runtime){
    if(runtime->has_supervisor && (!parent_supervisor_all_reaped(&runtime->supervisor) ||
        !parent_supervisor_cleanup_finished(&runtime->supervisor))){errno=EBUSY;return -1;}
    int result=0;
    if(runtime->has_loop)destroy_parent_http_loop(&runtime->loop);
    if(runtime->has_supervisor && destroy_parent_worker_supervisor(&runtime->supervisor)<0)result=-1;
    if(runtime->has_queue && destroy_parent_client_queue(&runtime->queue)<0)result=-1;
    if(runtime->has_ledger)destroy_parent_connection_ledger(&runtime->ledger);
    if(runtime->has_shared && destroy_shared_backend_memory(&runtime->shared)<0)result=-1;
    for(size_t i=0;i<2;++i)if(runtime->failure[i]>=0)close(runtime->failure[i]);
    return result;
}
int run_process_http_server(int listener,const struct proxy_config *config,struct process_signal_wait *signals){
    struct process_http_runtime runtime={.failure={-1,-1}};int result=1;
    if(prepare_runtime(&runtime,listener,config,signals)<0){perror("prepare concurrent server");goto cleanup;}
    if(start_network_workers(&runtime,listener,config,signals)<0){
        int saved=errno;uint64_t now=runtime.supervisor.last_now_ms;
        read_parent_supervisor_time_ms(&now);
        request_parent_supervisor_shutdown(&runtime.supervisor,now,PARENT_STOP_SYSTEM_ERROR,true);
        errno=saved;perror("start network workers");
    }
    close(runtime.failure[1]);runtime.failure[1]=-1;
    result=run_parent_http_loop(&runtime.loop);
    if(result){
        const struct parent_supervisor_diagnostic *d=&runtime.supervisor.diagnostic;
        fprintf(stderr,"server stopped: worker=%u generation=%llu operation=%u errno=%d check=%u recovery=%u/%u\n",
            d->worker_index,(unsigned long long)d->worker_generation,(unsigned int)d->failed_operation,d->system_error,
            (unsigned int)d->backend.failed_check,d->backend.recovery_phase,d->backend.recovery_stage);
    }
cleanup:
    if(cleanup_runtime(&runtime)<0){perror("concurrent server cleanup");result=1;}
    return result<0?1:result;
}
