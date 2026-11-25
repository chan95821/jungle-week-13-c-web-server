#define _POSIX_C_SOURCE 200809L
#include "parent_http_loop.h"
#include "worker_failure_channel.h"
#include "concurrency_limits.h"
#include "socket_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PARENT_LISTENER_EVENT UINT64_MAX
#define PARENT_FAILURE_EVENT (UINT64_MAX-1)
static int update_interest(int epoll_fd,int fd,uint32_t *current,uint32_t wanted,uint64_t token){
    if(*current==wanted)return 0;
    struct epoll_event event={.events=wanted,.data.u64=token};
    int operation=!wanted?EPOLL_CTL_DEL:!*current?EPOLL_CTL_ADD:EPOLL_CTL_MOD;
    if(epoll_ctl(epoll_fd,operation,fd,wanted?&event:NULL)<0)return -1;
    *current=wanted;return 0;
}
static void request_runtime_shutdown(struct parent_http_loop *loop,uint64_t now){
    request_parent_supervisor_shutdown(loop->supervisor,now,PARENT_STOP_SYSTEM_ERROR,true);
}
static int stop_channel_worker(struct parent_http_loop *loop,size_t index,uint64_t now){
    return request_parent_worker_stop(loop->supervisor,(uint32_t)index,now,PARENT_STOP_CHANNEL);
}
int create_parent_http_loop(int listener,int failure,struct parent_worker_supervisor *supervisor,
    const struct proxy_config *config,struct process_signal_wait *signals,struct parent_http_loop *loop){
    if(listener<0 || failure<0 || !supervisor || !supervisor->workers || !config || !signals || !signals->active || !loop){errno=EINVAL;return -1;}
    if(loop->channels){errno=EBUSY;return -1;}
    *loop=(struct parent_http_loop){.epoll_fd=-1,.listener_fd=listener,.failure_fd=failure,
        .supervisor=supervisor,.config=config,.signals=signals};
    loop->channels=calloc(supervisor->capacity,sizeof(*loop->channels));loop->available=calloc(supervisor->capacity,sizeof(*loop->available));
    if(!loop->channels || !loop->available)goto error;
    for(size_t i=0;i<supervisor->capacity;++i)loop->channels[i].registered_fd=-1;
    int flags=fcntl(listener,F_GETFL);
    if(flags<0 || fcntl(listener,F_SETFL,flags|O_NONBLOCK)<0 || set_close_on_exec(listener)<0)goto error;
    loop->epoll_fd=epoll_create1(EPOLL_CLOEXEC);if(loop->epoll_fd<0)goto error;
    struct epoll_event event={.events=EPOLLIN,.data.u64=PARENT_FAILURE_EVENT};
    if(epoll_ctl(loop->epoll_fd,EPOLL_CTL_ADD,failure,&event)<0)goto error;
    return 0;
error:{int error=errno;destroy_parent_http_loop(loop);errno=error;return -1;}
}
static int refresh_interests(struct parent_http_loop *loop){
    struct parent_worker_supervisor *supervisor=loop->supervisor;
    bool allow=parent_supervisor_accepts_clients(supervisor) && parent_accept_has_capacity(supervisor->queue,
        supervisor->ledger->total,supervisor->ledger->capacity,supervisor->stopping);
    if(update_interest(loop->epoll_fd,loop->listener_fd,&loop->listener_interest,allow?EPOLLIN:0,PARENT_LISTENER_EVENT)<0)return -1;
    for(size_t i=0;i<supervisor->capacity;++i){
        struct parent_supervised_worker *worker=&supervisor->workers[i];struct parent_http_channel *channel=&loop->channels[i];
        if(worker->channel_fd!=channel->registered_fd || worker->generation!=channel->generation){
            /* Reaped worker close removes its epoll registration. Never DEL a
             * recycled numeric FD from a stale record. No automatic respawn. */
            channel->registered_fd=worker->channel_fd;channel->generation=worker->generation;channel->interest=0;
            if(worker->channel_fd<0){channel->sending_connection=0;continue;}
        }
        if(worker->channel_fd<0)continue;
        bool serving=worker->phase==PARENT_WORKER_RUNNING;
        uint32_t wanted=serving?EPOLLIN:0;
        if(serving && channel->sending_connection)wanted|=EPOLLOUT;
        if(update_interest(loop->epoll_fd,worker->channel_fd,&channel->interest,wanted,i+1)<0)return -1;
    }
    return 0;
}
static int advance_sender(struct parent_http_loop *loop,size_t index,uint64_t now){
    struct parent_supervised_worker *worker=&loop->supervisor->workers[index];
    struct parent_http_channel *channel=&loop->channels[index];
    if(!channel->sending_connection || worker->phase!=PARENT_WORKER_RUNNING)return 0;
    int result=advance_worker_fd_send(worker->channel_fd,&worker->sender);
    if(result<0)return stop_channel_worker(loop,index,now);
    if(result==WORKER_FD_READY){
        if(finish_parent_connection_handoff(loop->supervisor->ledger,channel->sending_connection)<0)return -1;
        channel->sending_connection=0;
    }
    return 0;
}
static int dispatch_queued_clients(struct parent_http_loop *loop,uint64_t now){
    struct parent_worker_supervisor *supervisor=loop->supervisor;
    for(unsigned int issued=0;issued<WORKER_EVENT_CALLS && supervisor->queue->count && !supervisor->stopping;++issued){
        for(size_t i=0;i<supervisor->capacity;++i)loop->available[i]=parent_worker_accepts_assignment(supervisor,(uint32_t)i) &&
            !loop->channels[i].sending_connection && !supervisor->workers[i].sender.owns_client_fd;
        struct parent_queued_client front=supervisor->queue->clients[supervisor->queue->head];uint32_t worker;
        if(select_and_assign_parent_connection(supervisor->ledger,front.connection_id,loop->config->worker_policy,loop->available,&worker)<0){
            if(errno==EAGAIN)return 0;
            return -1;
        }
        struct parent_queued_client client={.client_fd=-1};
        if(dequeue_parent_client(supervisor->queue,&client)!=1)return -1;
        struct worker_client_metadata metadata={worker,supervisor->workers[worker].generation,client.connection_id,client.accepted_at};
        if(prepare_worker_fd_transfer(&supervisor->workers[worker].sender,&client.client_fd,&metadata)<0){
            int error=errno;close(client.client_fd);
            if(cancel_untransferred_parent_connection(supervisor->ledger,client.connection_id)<0)return -1;
            if(error==ENOTCONN || error==ECONNRESET || error==ECONNABORTED)continue;
            errno=error;return -1;
        }
        loop->channels[worker].sending_connection=client.connection_id;
        if(advance_sender(loop,worker,now)<0)return -1;
    }
    return 0;
}
static int accept_pending_clients(struct parent_http_loop *loop){
    struct parent_worker_supervisor *supervisor=loop->supervisor;
    for(unsigned int accepted=0;accepted<WORKER_EVENT_CALLS && parent_supervisor_accepts_clients(supervisor) &&
        parent_accept_has_capacity(supervisor->queue,supervisor->ledger->total,supervisor->ledger->capacity,supervisor->stopping);++accepted){
        int fd=accept(loop->listener_fd,NULL,NULL);
        if(fd<0){if(errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK || errno==ECONNABORTED)return 0;return -1;}
        struct timespec at;uint64_t id;
        if(set_close_on_exec(fd)<0 || clock_gettime(CLOCK_MONOTONIC,&at)<0 || register_parent_connection(supervisor->ledger,&id)<0){close(fd);return -1;}
        if(enqueue_parent_client(supervisor->queue,&fd,id,&at)<0){int error=errno;close(fd);cancel_queued_parent_connection(supervisor->ledger,id);errno=error;return -1;}
    }
    return 0;
}
static int read_worker_completions(struct parent_http_loop *loop,size_t index,uint64_t now){
    struct parent_supervised_worker *worker=&loop->supervisor->workers[index];
    if(worker->phase!=PARENT_WORKER_RUNNING)return 0;
    for(unsigned int i=0;i<WORKER_EVENT_CALLS;++i){
        struct worker_connection_closed completed;
        int result=advance_worker_closed_receive(worker->channel_fd,(uint32_t)index,worker->generation,&loop->channels[index].completed,&completed);
        if(result<0 || result==WORKER_CLOSED_EOF)return stop_channel_worker(loop,index,now);
        if(result==WORKER_CLOSED_PENDING)return 0;
        if(apply_worker_connection_observed_closed(loop->supervisor->ledger,(uint32_t)index,worker->generation,
            completed.connection_id,&completed.observation)<0)return stop_channel_worker(loop,index,now);
    }
    return 0;
}
static bool only_lost_workers_remain(const struct parent_worker_supervisor *supervisor){
    bool lost=false;
    for(size_t i=0;i<supervisor->capacity;++i){
        enum parent_worker_phase phase=supervisor->workers[i].phase;
        if(phase==PARENT_WORKER_LOST)lost=true;
        else if(phase!=PARENT_WORKER_UNUSED && phase!=PARENT_WORKER_FINISHED)return false;
    }
    return lost;
}
/* Terminal epoll failure only: preserve chosen signal masks and existing stop/
 * retry deadlines with a no-FD pselect wait, not a running-service poll fallback. */
static void wait_shutdown_control(struct parent_http_loop *loop,int timeout){
    struct timespec duration={0};struct timespec *limit=NULL;
    if(timeout>=0){duration.tv_sec=timeout/1000;duration.tv_nsec=(timeout%1000)*1000000L;limit=&duration;}
    pselect(0,NULL,NULL,NULL,limit,&loop->signals->wait_mask);
}
int run_parent_http_loop(struct parent_http_loop *loop){
    if(!loop || !loop->channels || !loop->available || loop->epoll_fd<0 ||
        (!loop->supervisor->started && !loop->supervisor->stopping)){errno=EINVAL;return -1;}
    struct parent_worker_supervisor *supervisor=loop->supervisor;bool failed_epoll=false;
    for(;;){
        uint64_t now;if(read_parent_supervisor_time_ms(&now)<0)return 1;
        if(process_signal_shutdown_requested(loop->signals) && !supervisor->stopping)
            request_parent_supervisor_shutdown(supervisor,now,PARENT_STOP_REQUEST,false);
        if(atomic_load(&supervisor->backend->header->fatal))
            request_parent_supervisor_shutdown(supervisor,now,PARENT_STOP_SHARED_FATAL,true);
        take_process_child_signal(loop->signals);
        if(advance_parent_worker_supervisor(supervisor,now)<0)return 1;
        if(parent_supervisor_cleanup_finished(supervisor))return supervisor->exit_error?1:0;
        if(supervisor->stopping && only_lost_workers_remain(supervisor))return 1;
        if(!supervisor->stopping && dispatch_queued_clients(loop,now)<0)request_runtime_shutdown(loop,now);
        if(!failed_epoll && refresh_interests(loop)<0){request_runtime_shutdown(loop,now);failed_epoll=true;}
        int timeout=parent_supervisor_wait_timeout_ms(supervisor,now);
        if(failed_epoll){wait_shutdown_control(loop,timeout);continue;}
        struct epoll_event events[WORKER_EPOLL_BATCH];
        int count=wait_process_signal_events(loop->signals,loop->epoll_fd,events,WORKER_EPOLL_BATCH,timeout);
        if(count<0){if(errno==EINTR)continue;request_runtime_shutdown(loop,now);failed_epoll=true;continue;}
        if(read_parent_supervisor_time_ms(&now)<0)return 1;
        /* Do not accept/dispatch from a ready batch before stop/fatal is handled. */
        if(process_signal_shutdown_requested(loop->signals) || atomic_load(&supervisor->backend->header->fatal))continue;
        for(int i=0;i<count && !supervisor->stopping;++i){
            uint64_t token=events[i].data.u64;uint32_t ready=events[i].events;int result=0;
            if(token==PARENT_LISTENER_EVENT)result=accept_pending_clients(loop);
            else if(token==PARENT_FAILURE_EVENT){int status=consume_worker_failure_wakeup(loop->failure_fd);if(status<0 || status==WORKER_FAILURE_WAKEUP_EOF)result=-1;}
            else if(token && token<=supervisor->capacity){size_t index=(size_t)token-1;
                if(supervisor->workers[index].phase!=PARENT_WORKER_RUNNING)continue;
                if(ready&(EPOLLERR|EPOLLHUP))result=stop_channel_worker(loop,index,now);
                else {if(ready&EPOLLIN)result=read_worker_completions(loop,index,now);
                    if(!result && (ready&EPOLLOUT))result=advance_sender(loop,index,now);}
            }
            if(result<0)request_runtime_shutdown(loop,now);
        }
    }
}
void destroy_parent_http_loop(struct parent_http_loop *loop){
    if(!loop)return;
    if(loop->channels && loop->epoll_fd>=0)close(loop->epoll_fd);
    free(loop->channels);free(loop->available);
    *loop=(struct parent_http_loop){.epoll_fd=-1,.listener_fd=-1,.failure_fd=-1};
}
