#define _GNU_SOURCE
#include "../../src/parent_worker_supervisor.h"
#include "../../src/process_signal_wait.h"
#include "../../src/worker_closed_channel.h"
#include "../../src/concurrency_limits.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t signal_state;
static int wait_error_once,kill_error_once;
static unsigned int term_calls,kill_calls;
static size_t fd_send_limit;
static size_t backend_trylock_calls,wait_calls;
pid_t supervisor_test_waitpid(pid_t pid,int *status,int options) {
    assert(pid>0 && options==WNOHANG);++wait_calls;
    if(wait_error_once) {errno=wait_error_once;wait_error_once=0;return -1;}
    return waitpid(pid,status,options);
}
int supervisor_test_kill(pid_t pid,int number) {
    assert(pid>0);
    if(number==SIGTERM) ++term_calls; else {assert(number==SIGKILL);++kill_calls;}
    if(kill_error_once) {errno=kill_error_once;kill_error_once=0;return -1;}
    return kill(pid,number);
}
ssize_t worker_test_sendmsg(int fd,const struct msghdr *message,int flags) {
    struct msghdr limited=*message;struct iovec vector=message->msg_iov[0];
    if(fd_send_limit && vector.iov_len>fd_send_limit) vector.iov_len=fd_send_limit;
    limited.msg_iov=&vector;return sendmsg(fd,&limited,flags);
}
ssize_t worker_test_recvmsg(int fd,struct msghdr *message,int flags) {return recvmsg(fd,message,flags);}
int supervisor_test_backend_trylock(struct shared_backend_memory *memory,struct backend_state_diagnostic *diagnostic) {
    ++backend_trylock_calls;return try_lock_shared_backend_state(memory,diagnostic);
}
static void on_signal(int number) {record_process_signal(&signal_state,number);}
static uint64_t now_ms(void) {uint64_t now;assert(read_parent_supervisor_time_ms(&now)==0);return now;}
static size_t fd_count(void) {
    DIR *directory=opendir("/proc/self/fd");assert(directory);size_t count=0;struct dirent *entry;
    while((entry=readdir(directory))) if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    assert(closedir(directory)==0);return count;
}
enum child_mode {CHILD_ECHO,CHILD_IDLE,CHILD_IGNORE_TERM,CHILD_HOLD_LOCK};
struct fixture {
    struct shared_backend_memory backend;
    struct parent_connection_ledger ledger;
    struct parent_client_queue queue;
    struct parent_worker_supervisor supervisor;
    struct process_signal_wait signals;
    int epoll_fd;
};
static void child_loop(struct fixture *fixture,size_t worker,int channel,int ready,enum child_mode mode) {
    alarm(15); /* Test-only orphan/hang guard, not a production lifetime policy. */
    for(size_t i=0;i<fixture->supervisor.capacity;++i)
        if(fixture->supervisor.workers[i].channel_fd>=0) close(fixture->supervisor.workers[i].channel_fd);
    close(fixture->epoll_fd);
    struct sigaction action={.sa_handler=mode==CHILD_IGNORE_TERM?SIG_IGN:SIG_DFL};sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL)<0) _exit(2);
    sigset_t unblocked;sigemptyset(&unblocked);if(sigprocmask(SIG_SETMASK,&unblocked,NULL)<0)_exit(3);
    struct backend_state_diagnostic diagnostic;
    if(mode==CHILD_HOLD_LOCK && try_lock_shared_backend_state(&fixture->backend,&diagnostic)<0)_exit(4);
    if(write(ready,"R",1)!=1)_exit(5);
    close(ready);
    (void)worker;
    if(mode==CHILD_IDLE || mode==CHILD_IGNORE_TERM) for(;;) pause();
    for(;;) {
        struct pollfd watch={.fd=channel,.events=POLLIN};
        if(poll(&watch,1,-1)<0) {if(errno==EINTR)continue;_exit(6);}
        unsigned char buffer[64];ssize_t count=recv(channel,buffer,sizeof(buffer),0);
        if(count<0) {if(errno==EINTR || errno==EAGAIN)continue;_exit(7);}
        if(!count)_exit(0);
        if(mode==CHILD_HOLD_LOCK) {if(unlock_shared_backend_state(&fixture->backend)<0)_exit(8);mode=CHILD_ECHO;}
        if(send(channel,buffer,(size_t)count,MSG_NOSIGNAL)!=count)_exit(9);
    }
}
static void create_fixture(struct fixture *fixture,size_t workers,const enum child_mode *modes) {
    *fixture=(struct fixture){0};term_calls=kill_calls=0;wait_error_once=kill_error_once=0;fd_send_limit=0;backend_trylock_calls=wait_calls=0;
    assert(prepare_process_signal_wait(&fixture->signals,&signal_state,on_signal)==0);
    fixture->epoll_fd=epoll_create1(EPOLL_CLOEXEC);assert(fixture->epoll_fd>=0);
    assert(create_shared_backend_memory(workers,16,1024*1024,&fixture->backend)==0);
    assert(create_parent_connection_ledger(8,workers,4,&fixture->ledger)==0);
    assert(create_parent_client_queue(4,&fixture->queue)==0);
    struct backend_state_diagnostic diagnostic;
    assert(try_lock_shared_backend_state(&fixture->backend,&diagnostic)==0);
    for(uint32_t worker=0;worker<workers;++worker) {
        uint64_t generation;assert(activate_backend_worker_locked(&fixture->backend,worker,&generation,&diagnostic)==0 && generation==1);
        assert(activate_parent_connection_worker(&fixture->ledger,worker,generation)==0);
    }
    assert(unlock_shared_backend_state(&fixture->backend)==0);
    assert(create_parent_worker_supervisor(&fixture->ledger,&fixture->backend,&fixture->queue,&fixture->supervisor)==0);
    for(uint32_t worker=0;worker<workers;++worker) {
        int channel[2],ready[2];assert(create_worker_fd_channel(channel)==0 && pipe(ready)==0);
        pid_t child=fork();assert(child>=0);
        if(!child) {close(channel[0]);close(ready[0]);child_loop(fixture,worker,channel[1],ready[1],modes[worker]);_exit(99);}
        close(channel[1]);close(ready[1]);char marker;assert(read(ready[0],&marker,1)==1);close(ready[0]);
        assert(adopt_parent_worker(&fixture->supervisor,worker,1,child,&channel[0])==0 && channel[0]==-1);
    }
    assert(finish_parent_worker_startup(&fixture->supervisor)==0);
    assert(destroy_parent_worker_supervisor(&fixture->supervisor)==-1 && errno==EBUSY);
}
static void wait_signal_turn(struct fixture *fixture,int timeout) {
    struct epoll_event event;int result=wait_process_signal_events(&fixture->signals,fixture->epoll_fd,&event,1,timeout);
    assert(result>=0 || errno==EINTR);
}
/* Use production deadlines, capped only by the test hang guard. */
static void pump_worker(struct fixture *fixture,uint32_t worker,bool cleanup) {
    uint64_t end=now_ms()+5000;
    while(cleanup?fixture->supervisor.workers[worker].phase!=PARENT_WORKER_FINISHED:!fixture->supervisor.workers[worker].reaped) {
        assert(now_ms()<end);assert(advance_parent_worker_supervisor(&fixture->supervisor,now_ms())>=0);
        if(cleanup?fixture->supervisor.workers[worker].phase==PARENT_WORKER_FINISHED:fixture->supervisor.workers[worker].reaped)break;
        uint64_t now=now_ms();assert(now<end);
        int timeout=parent_supervisor_wait_timeout_ms(&fixture->supervisor,now);
        int guard=(int)(end-now);if(timeout<0 || timeout>guard)timeout=guard;
        wait_signal_turn(fixture,timeout);
    }
}
static void destroy_fixture(struct fixture *fixture) {
    if(!parent_supervisor_cleanup_finished(&fixture->supervisor)) {
        assert(request_parent_supervisor_shutdown(&fixture->supervisor,now_ms(),PARENT_STOP_REQUEST,false)==0);
        for(uint32_t worker=0;worker<fixture->supervisor.capacity;++worker) pump_worker(fixture,worker,true);
    }
    assert(parent_supervisor_all_reaped(&fixture->supervisor));
    assert(destroy_parent_worker_supervisor(&fixture->supervisor)==0);
    assert(destroy_parent_worker_supervisor(&fixture->supervisor)==0);
    assert(destroy_parent_client_queue(&fixture->queue)==0);destroy_parent_connection_ledger(&fixture->ledger);
    assert(destroy_shared_backend_memory(&fixture->backend)==0);
    close(fixture->epoll_fd);assert(restore_process_signal_wait(&fixture->signals)==0);
}
static uint64_t assign_connection(struct fixture *fixture,uint32_t worker) {
    uint64_t id;assert(register_parent_connection(&fixture->ledger,&id)==0);
    assert(assign_parent_connection(&fixture->ledger,id,worker,1)==0);return id;
}
static void reserve_backend(struct fixture *fixture,uint32_t worker,uint32_t slot,uint32_t backend) {
    struct backend_state_diagnostic diagnostic;struct backend_request_token token;
    assert(try_lock_shared_backend_state(&fixture->backend,&diagnostic)==0);
    assert(reserve_backend_request_locked(&fixture->backend,worker,1,slot,backend,0,&token,&diagnostic)==0);
    assert(unlock_shared_backend_state(&fixture->backend)==0);
}
static void echo_worker(struct fixture *fixture,uint32_t worker) {
    int channel=fixture->supervisor.workers[worker].channel_fd;
    assert(send(channel,"OK",2,MSG_NOSIGNAL)==2);
    struct pollfd watch={.fd=channel,.events=POLLIN};assert(poll(&watch,1,1000)==1);
    char bytes[2];assert(recv(channel,bytes,2,0)==2 && !memcmp(bytes,"OK",2));
}
static void test_channel_isolation(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_ECHO,CHILD_ECHO};create_fixture(&fixture,2,modes);
    uint64_t id=assign_connection(&fixture,0);assert(finish_parent_connection_handoff(&fixture.ledger,id)==0);
    uint64_t other=assign_connection(&fixture,1);assert(finish_parent_connection_handoff(&fixture.ledger,other)==0);
    reserve_backend(&fixture,0,0,1);reserve_backend(&fixture,1,1,2);
    int channel=fixture.supervisor.workers[0].channel_fd;char bad[WORKER_CLOSED_FRAME_BYTES]={0};assert(send(channel,bad,sizeof(bad),0)==sizeof(bad));
    struct pollfd watch={.fd=channel,.events=POLLIN};assert(poll(&watch,1,1000)==1);
    struct worker_closed_receiver receiver={0};struct worker_connection_closed closed={0};
    assert(advance_worker_closed_receive(channel,0,1,&receiver,&closed)==-1 && errno==EPROTO);
    assert(request_parent_worker_stop(&fixture.supervisor,0,now_ms(),PARENT_STOP_CHANNEL)==0);
    assert(!parent_worker_accepts_assignment(&fixture.supervisor,0) && parent_supervisor_accepts_clients(&fixture.supervisor));
    pump_worker(&fixture,0,true);echo_worker(&fixture,1);
    assert(!fixture.supervisor.stopping && !fixture.supervisor.exit_error && term_calls==1 && kill_calls==0);
    assert(fixture.ledger.total==1 && fixture.ledger.workers[1].outstanding==1);
    assert(fixture.backend.header->outstanding[1]==0 && fixture.backend.header->outstanding[2]==1);
    assert(apply_worker_connection_closed(&fixture.ledger,0,1,id)==1);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0 && term_calls==1);
    destroy_fixture(&fixture);
}
static void test_stop_deadline(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IGNORE_TERM};create_fixture(&fixture,1,modes);
    uint64_t start=now_ms();assert(request_parent_worker_stop(&fixture.supervisor,0,start,PARENT_STOP_CHANNEL)==0);
    assert(request_parent_worker_stop(&fixture.supervisor,0,start,PARENT_STOP_CHANNEL)==0);
    assert(fixture.supervisor.workers[0].stop_deadline_ms==start+2000);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start)==0 && term_calls==1 && kill_calls==0);
    assert(fixture.supervisor.stopping && fixture.supervisor.exit_error); /* Last viable worker lost. */
    assert(parent_supervisor_signal_timeout_ms(&fixture.supervisor,start+1999)==1);
    assert(parent_supervisor_signal_timeout_ms(&fixture.supervisor,start+2000)==0);
    while(now_ms()<start+INITIAL_WORKER_STOP_GRACE_MS) {
        int timeout=parent_supervisor_signal_timeout_ms(&fixture.supervisor,now_ms());
        wait_signal_turn(&fixture,timeout);
    }
    assert(kill_calls==0 && !fixture.supervisor.workers[0].reaped);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0 && kill_calls==1);
    assert(!parent_supervisor_all_reaped(&fixture.supervisor));pump_worker(&fixture,0,true);
    assert(WIFSIGNALED(fixture.supervisor.workers[0].wait_status) && WTERMSIG(fixture.supervisor.workers[0].wait_status)==SIGKILL);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0 && term_calls==1 && kill_calls==1);
    destroy_fixture(&fixture);
}
static void test_partial_sender_cleanup(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_ECHO};create_fixture(&fixture,2,modes);
    uint64_t id=assign_connection(&fixture,0);reserve_backend(&fixture,0,0,0);
    int client[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,client)==0);
    struct worker_client_metadata meta={0,1,id,{.tv_sec=1}};
    struct parent_supervised_worker *worker=&fixture.supervisor.workers[0];
    assert(prepare_worker_fd_transfer(&worker->sender,&client[0],&meta)==0);
    fd_send_limit=1;assert(advance_worker_fd_send(worker->channel_fd,&worker->sender)==WORKER_FD_PENDING);fd_send_limit=0;
    assert(worker->sender.fd_sent && worker->sender.owns_client_fd);
    assert(kill(worker->pid,SIGKILL)==0);pump_worker(&fixture,0,true);
    assert(!worker->sender.owns_client_fd && worker->channel_fd==-1 && fixture.ledger.total==0);
    char byte;assert(read(client[1],&byte,1)==0);close(client[1]);
    assert(fixture.backend.header->outstanding[0]==0);echo_worker(&fixture,1);destroy_fixture(&fixture);
}
static void test_busy_backend_cleanup(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_HOLD_LOCK};create_fixture(&fixture,2,modes);
    /* Worker 1 already owns mutex. No parent lock operation until it releases. */
    assign_connection(&fixture,0);assign_connection(&fixture,1);
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);pump_worker(&fixture,0,false);
    assert(fixture.supervisor.cleanup_pending && fixture.supervisor.workers[0].phase==PARENT_WORKER_REAPED);
    assert(fixture.ledger.total==1 && parent_supervisor_accepts_clients(&fixture.supervisor));
    assert(destroy_parent_worker_supervisor(&fixture.supervisor)==-1 && errno==EBUSY);
    echo_worker(&fixture,1); /* Its incoming command unlocks before echoing. */
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())>=0);
    pump_worker(&fixture,0,true);
    assert(fixture.supervisor.workers[0].phase==PARENT_WORKER_FINISHED && fixture.ledger.workers[1].outstanding==1);
    destroy_fixture(&fixture);
}
static void test_shutdown_and_queue(bool fatal) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_ECHO};create_fixture(&fixture,2,modes);
    uint64_t id;int client[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,client)==0);
    assert(register_parent_connection(&fixture.ledger,&id)==0);struct timespec accepted={.tv_sec=1};
    assert(enqueue_parent_client(&fixture.queue,&client[0],id,&accepted)==0);
    assign_connection(&fixture,0);reserve_backend(&fixture,0,0,0);
    if(fatal) atomic_store(&fixture.backend.header->fatal,1);
    else {
        assert(kill(getpid(),SIGTERM)==0);wait_signal_turn(&fixture,0);
        assert(process_signal_shutdown_requested(&fixture.signals));
        assert(request_parent_supervisor_shutdown(&fixture.supervisor,now_ms(),PARENT_STOP_REQUEST,false)==0);
    }
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0);
    assert(fixture.supervisor.stopping && !parent_supervisor_accepts_clients(&fixture.supervisor));
    assert(fixture.queue.count==0 && fixture.ledger.queued==0);
    char byte;assert(read(client[1],&byte,1)==0);close(client[1]);
    for(uint32_t worker=0;worker<2;++worker) pump_worker(&fixture,worker,true);
    assert(fixture.ledger.total==0 && fixture.supervisor.exit_error==fatal);
    if(fatal) assert(fixture.backend.header->outstanding[0]==1); /* Retain untrusted evidence, no reset. */
    destroy_fixture(&fixture);
}
static void test_corruption_shutdown(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_IDLE};create_fixture(&fixture,2,modes);
    reserve_backend(&fixture,0,0,1);
    struct backend_state_diagnostic diagnostic;assert(try_lock_shared_backend_state(&fixture.backend,&diagnostic)==0);
    ++fixture.backend.header->outstanding[1];assert(unlock_shared_backend_state(&fixture.backend)==0);
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);pump_worker(&fixture,0,true);
    assert(fixture.supervisor.stopping && fixture.supervisor.exit_error && atomic_load(&fixture.backend.header->fatal));
    assert(fixture.supervisor.diagnostic.failed_operation==PARENT_SUPERVISOR_BACKEND);
    assert(fixture.supervisor.diagnostic.worker_index==0 && fixture.supervisor.diagnostic.worker_generation==1);
    assert(fixture.supervisor.diagnostic.backend.failed_check!=BACKEND_CHECK_NONE);
    pump_worker(&fixture,1,true);destroy_fixture(&fixture);
}
static void test_syscall_errors(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_IDLE};create_fixture(&fixture,2,modes);
    wait_error_once=EINTR;assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==1);
    assert(!fixture.supervisor.stopping && parent_supervisor_accepts_clients(&fixture.supervisor));
    assert(request_parent_worker_stop(&fixture.supervisor,0,now_ms(),PARENT_STOP_CHANNEL)==0);
    kill_error_once=ESRCH;assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())>=0);
    assert(!fixture.supervisor.workers[0].reaped && !fixture.supervisor.stopping); /* ESRCH is not waitpid proof. */
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);pump_worker(&fixture,0,true);
    destroy_fixture(&fixture);
    create_fixture(&fixture,2,modes);
    assert(request_parent_worker_stop(&fixture.supervisor,0,now_ms(),PARENT_STOP_CHANNEL)==0);
    kill_error_once=EPERM;assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0);
    assert(fixture.supervisor.stopping && fixture.supervisor.exit_error);
    assert(fixture.supervisor.diagnostic.failed_operation==PARENT_SUPERVISOR_SIGNAL);
    /* Test teardown kills the child whose injected TERM was not delivered. */
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);
    destroy_fixture(&fixture);
}
static void test_dead_mutex_owner(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_HOLD_LOCK,CHILD_ECHO};create_fixture(&fixture,2,modes);
    assign_connection(&fixture,0);assign_connection(&fixture,1);
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);pump_worker(&fixture,0,true);
    assert(!atomic_load(&fixture.backend.header->fatal) && !fixture.supervisor.stopping);
    assert(fixture.backend.workers[0].state==BACKEND_WORKER_RETIRED && fixture.backend.workers[1].state==BACKEND_WORKER_ACTIVE);
    echo_worker(&fixture,1);destroy_fixture(&fixture);
}
static void test_lost_wait_ownership(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE};create_fixture(&fixture,1,modes);
    pid_t pid=fixture.supervisor.workers[0].pid;assert(kill(pid,SIGKILL)==0);
    int actual_status;assert(waitpid(pid,&actual_status,0)==pid); /* Deliberately violate single-reaper contract. */
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0);
    assert(fixture.supervisor.workers[0].phase==PARENT_WORKER_LOST && !fixture.supervisor.workers[0].reaped);
    assert(fixture.supervisor.stopping && fixture.supervisor.exit_error && !parent_supervisor_all_reaped(&fixture.supervisor));
    assert(fixture.supervisor.diagnostic.failed_operation==PARENT_SUPERVISOR_WAIT && fixture.supervisor.diagnostic.system_error==ECHILD);
    assert(destroy_parent_worker_supervisor(&fixture.supervisor)==-1 && errno==EBUSY);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0 && !term_calls && !kill_calls);
    /* Test-only teardown has the actual external wait result above. Production
     * must not infer death/release from ECHILD or rewrite confirmation like this. */
    close(fixture.supervisor.workers[0].channel_fd);
    free(fixture.supervisor.workers);fixture.supervisor=(struct parent_worker_supervisor){0};
    assert(destroy_parent_client_queue(&fixture.queue)==0);destroy_parent_connection_ledger(&fixture.ledger);
    assert(destroy_shared_backend_memory(&fixture.backend)==0);
    close(fixture.epoll_fd);assert(restore_process_signal_wait(&fixture.signals)==0);
}

static void test_last_worker_exit(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE};create_fixture(&fixture,1,modes);
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);pump_worker(&fixture,0,true);
    assert(fixture.supervisor.stopping && fixture.supervisor.exit_error && !parent_supervisor_accepts_clients(&fixture.supervisor));
    assert(fixture.supervisor.workers[0].phase==PARENT_WORKER_FINISHED);destroy_fixture(&fixture);
}
/* Deterministic input clock for retry rate limiting; process death is real. */
static void reap_at_time(struct fixture *fixture,uint32_t worker,uint64_t clock_ms) {
    uint64_t guard=now_ms()+3000;
    while(!fixture->supervisor.workers[worker].reaped) {
        assert(now_ms()<guard);
        assert(advance_parent_worker_supervisor(&fixture->supervisor,clock_ms)>=0);
        if(!fixture->supervisor.workers[worker].reaped)wait_signal_turn(fixture,1);
    }
}
static void catch_up_real_clock(struct fixture *fixture) {
    for(;;) {
        uint64_t now=now_ms();if(now>=fixture->supervisor.last_now_ms)break;
        wait_signal_turn(fixture,(int)(fixture->supervisor.last_now_ms-now));
    }
}
static void test_retry_timer_without_new_io(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_HOLD_LOCK};create_fixture(&fixture,2,modes);
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);
    uint64_t start=now_ms();reap_at_time(&fixture,0,start);
    assert(fixture.supervisor.workers[0].retry_kind==PARENT_RETRY_BACKEND);
    assert(fixture.supervisor.workers[0].retry_at_ms==start+INITIAL_SUPERVISOR_RETRY_MS);
    size_t calls=backend_trylock_calls;
    /* Actual always-ready LT pipe must not make cleanup spin on the mutex. */
    int ready[2];assert(pipe(ready)==0);assert(write(ready[1],"R",1)==1);
    struct epoll_event registration={.events=EPOLLIN};
    assert(epoll_ctl(fixture.epoll_fd,EPOLL_CTL_ADD,ready[0],&registration)==0);
    for(size_t i=0;i<16;++i) {
        wait_signal_turn(&fixture,0);
        assert(advance_parent_worker_supervisor(&fixture.supervisor,start)==1);
    }
    assert(backend_trylock_calls==calls);
    close(ready[0]);close(ready[1]);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start+9)==1);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start+10)==0);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start+11)==0);
    echo_worker(&fixture,1); /* Unlock + final I/O. No more requests or notify pipe. */
    catch_up_real_clock(&fixture);
    while(now_ms()<fixture.supervisor.workers[0].retry_at_ms)
        wait_signal_turn(&fixture,parent_supervisor_wait_timeout_ms(&fixture.supervisor,now_ms()));
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0);
    assert(backend_trylock_calls==calls+1 && fixture.supervisor.workers[0].phase==PARENT_WORKER_FINISHED);
    assert(!fixture.supervisor.cleanup_pending && parent_supervisor_wait_timeout_ms(&fixture.supervisor,now_ms())==-1);
    destroy_fixture(&fixture);
}
static void test_independent_retry_deadlines(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_IDLE,CHILD_HOLD_LOCK};create_fixture(&fixture,3,modes);
    uint64_t start=now_ms();
    assert(kill(fixture.supervisor.workers[0].pid,SIGKILL)==0);reap_at_time(&fixture,0,start);
    assert(kill(fixture.supervisor.workers[1].pid,SIGKILL)==0);reap_at_time(&fixture,1,start+5);
    assert(fixture.supervisor.workers[0].retry_at_ms==start+10);
    assert(fixture.supervisor.workers[1].retry_at_ms==start+15);
    size_t calls=backend_trylock_calls;
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start+9)==1 && backend_trylock_calls==calls);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start+10)==1 && backend_trylock_calls==calls+1);
    assert(fixture.supervisor.workers[0].retry_at_ms==start+20 && fixture.supervisor.workers[1].retry_at_ms==start+15);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start+14)==1);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start+15)==1 && backend_trylock_calls==calls+2);
    assert(fixture.supervisor.workers[0].retry_at_ms==start+20 && fixture.supervisor.workers[1].retry_at_ms==start+25);
    echo_worker(&fixture,2);catch_up_real_clock(&fixture);
    pump_worker(&fixture,0,true);pump_worker(&fixture,1,true);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,now_ms())==-1);
    destroy_fixture(&fixture);
}
static void test_wait_eintr_retry_without_signal(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE};create_fixture(&fixture,1,modes);
    pid_t child=fixture.supervisor.workers[0].pid;assert(kill(child,SIGKILL)==0);
    siginfo_t information;assert(waitid(P_PID,(id_t)child,&information,WEXITED|WNOWAIT)==0);
    wait_signal_turn(&fixture,0); /* Consume CHLD without reaping: no second signal needed. */
    uint64_t start=now_ms();wait_error_once=EINTR;
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start)==1);
    assert(fixture.supervisor.workers[0].retry_kind==PARENT_RETRY_WAIT && !fixture.supervisor.workers[0].reaped);
    size_t calls=wait_calls;
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start+9)==1 && wait_calls==calls);
    catch_up_real_clock(&fixture);
    while(now_ms()<start+10) wait_signal_turn(&fixture,parent_supervisor_wait_timeout_ms(&fixture.supervisor,now_ms()));
    assert(advance_parent_worker_supervisor(&fixture.supervisor,now_ms())==0);
    assert(wait_calls==calls+1 && fixture.supervisor.workers[0].reaped);
    assert(fixture.supervisor.workers[0].retry_kind==PARENT_RETRY_NONE && fixture.supervisor.exit_error);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,now_ms())==-1);destroy_fixture(&fixture);
}
static void test_timeout_math_and_fatal_priority(void) {
    struct fixture fixture;const enum child_mode modes[]={CHILD_IDLE,CHILD_HOLD_LOCK};create_fixture(&fixture,2,modes);
    struct parent_supervised_worker saved=fixture.supervisor.workers[0];uint64_t start=now_ms();
    /* Pure deadline calculation: no process action under synthetic state. */
    fixture.supervisor.workers[0].phase=PARENT_WORKER_STOPPING;
    fixture.supervisor.workers[0].stop_deadline_ms=start+3;
    fixture.supervisor.workers[0].retry_kind=PARENT_RETRY_WAIT;
    fixture.supervisor.workers[0].retry_at_ms=start+10;
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start)==3);
    assert(parent_supervisor_signal_timeout_ms(&fixture.supervisor,start)==3);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start+2)==1);
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start+3)==0);
    fixture.supervisor.workers[0].kill_attempted=true;
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start)==10);
    fixture.supervisor.workers[0].retry_kind=PARENT_RETRY_NONE;
    assert(parent_supervisor_wait_timeout_ms(&fixture.supervisor,start)==-1);
    fixture.supervisor.workers[0]=saved;
    assert(kill(saved.pid,SIGKILL)==0);reap_at_time(&fixture,0,start);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start-1)==-1 && errno==EINVAL);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,UINT64_MAX)==-1 && errno==EINVAL);
    size_t calls=backend_trylock_calls;atomic_store(&fixture.backend.header->fatal,1);
    assert(advance_parent_worker_supervisor(&fixture.supervisor,start)==0); /* Bypasses retry due time. */
    assert(fixture.supervisor.stopping && fixture.supervisor.workers[0].retry_kind==PARENT_RETRY_NONE);
    assert(fixture.supervisor.workers[0].phase==PARENT_WORKER_FINISHED && backend_trylock_calls==calls);
    assert(fixture.supervisor.workers[1].term_attempted);destroy_fixture(&fixture);
}

int main(void) {
    alarm(45);setvbuf(stdout,NULL,_IOLBF,0);size_t before=fd_count();
    test_channel_isolation();puts("[PASS] malformed control -> worker TERM/reap, healthy worker I/O and both ledgers preserved");
    test_stop_deadline();puts("[PASS] actual 2s grace with TERM ignored, SIGKILL once, waitpid before release");
    test_partial_sender_cleanup();puts("[PASS] partial FD sender and backend reservation reclaimed after actual SIGKILL");
    test_busy_backend_cleanup();puts("[PASS] mutex EBUSY defers cleanup while healthy worker/control progresses, resume after unlock");
    test_shutdown_and_queue(false);test_shutdown_and_queue(true);puts("[PASS] epoll_pwait TERM and shared fatal -> queue cleanup, all-worker reap, fatal evidence retained");
    test_corruption_shutdown();puts("[PASS] backend invariant failure -> bounded numeric diagnostic and all-worker stop");
    test_dead_mutex_owner();test_lost_wait_ownership();puts("[PASS] actual robust owner death recovery; ECHILD never authorizes signals or destruction");
    test_syscall_errors();test_last_worker_exit();puts("[PASS] wait EINTR, kill ESRCH/EPERM, last-worker loss, no respawn, FD cleanup");
    test_retry_timer_without_new_io();test_independent_retry_deadlines();
    puts("[PASS] per-worker 10ms retry, ready-I/O rate limiting, epoll timer completion without new requests");
    test_wait_eintr_retry_without_signal();test_timeout_math_and_fatal_priority();
    puts("[PASS] wait EINTR timer without new CHLD, minimum/expired/idle deadlines, clock guards, fatal priority");
    assert(fd_count()==before);alarm(0);
    printf("supervisor storage: owner=%zu worker=%zu diagnostic=%zu\n",sizeof(struct parent_worker_supervisor),sizeof(struct parent_supervised_worker),sizeof(struct parent_supervisor_diagnostic));
    return 0;
}
