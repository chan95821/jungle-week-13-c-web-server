#define _GNU_SOURCE
#include "../../src/backend_request_lifetime.h"
#include "../../src/parent_connection_ledger.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t fd_count(void) {
    DIR *directory=opendir("/proc/self/fd");assert(directory);size_t count=0;struct dirent *entry;
    while((entry=readdir(directory)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))++count;
    assert(closedir(directory)==0);return count;
}
static void create_memory(struct shared_backend_memory *memory) {
    *memory=(struct shared_backend_memory){0};assert(create_shared_backend_memory(2,4,1024*1024,memory)==0);
    struct backend_state_diagnostic diagnostic;assert(try_lock_shared_backend_state(memory,&diagnostic)==0);
    for(uint32_t worker=0;worker<2;++worker){uint64_t generation;assert(activate_backend_worker_locked(memory,worker,&generation,&diagnostic)==0 && generation==1);}
    assert(unlock_shared_backend_state(memory)==0);
}
static void begin(struct shared_backend_memory *memory,struct backend_request_lifetime *lifetime,
                   uint32_t worker,uint32_t slot,uint32_t backend) {
    struct backend_state_diagnostic diagnostic;assert(try_lock_shared_backend_state(memory,&diagnostic)==0);
    /* Selection result and reserve use the same held mutex. No I/O here. */
    assert(begin_backend_request_locked(memory,lifetime,worker,1,slot,backend,1,&diagnostic)==0);
    assert(memory->header->outstanding[backend]>0);
    assert(unlock_shared_backend_state(memory)==0);
}
static void validate(struct shared_backend_memory *memory) {
    struct backend_state_diagnostic diagnostic;assert(try_lock_shared_backend_state(memory,&diagnostic)==0);
    assert(validate_backend_reservations_locked(memory,&diagnostic)==0);assert(unlock_shared_backend_state(memory)==0);
}
static void test_transitions_and_outcomes(void) {
    struct shared_backend_memory memory;create_memory(&memory);struct backend_state_diagnostic diagnostic;
    struct backend_request_lifetime life={0};
    assert(mark_backend_request_finished(&life,BACKEND_OUTCOME_COMPLETE)==-1 && errno==EINVAL);
    assert(try_release_backend_request(&memory,&life,&diagnostic)==-1 && errno==EINVAL);
    assert(begin_backend_request_locked(&memory,&life,0,1,0,1,0,&diagnostic)==-1 && errno==EPERM);
    assert(life.phase==BACKEND_REQUEST_IDLE);
    for(int outcome=BACKEND_OUTCOME_COMPLETE;outcome<=BACKEND_OUTCOME_CANCELLED;++outcome) {
        begin(&memory,&life,0,0,1);
        assert(life.phase==BACKEND_REQUEST_ACTIVE && memory.header->outstanding[1]==1);
        assert(reset_backend_request_lifetime(&life)==-1 && errno==EBUSY);
        assert(try_release_backend_request(&memory,&life,&diagnostic)==-1 && errno==EINVAL);
        assert(mark_backend_request_finished(&life,(enum backend_request_outcome)outcome)==0);
        assert(memory.header->outstanding[1]==1 && life.phase==BACKEND_REQUEST_RELEASE_PENDING);
        assert(mark_backend_request_finished(&life,BACKEND_OUTCOME_FAILED)==1 && (int)life.outcome==outcome);
        assert(reset_backend_request_lifetime(&life)==-1 && errno==EBUSY);
        assert(begin_backend_request_locked(&memory,&life,0,1,1,2,0,&diagnostic)==-1 && errno==EBUSY);
        assert(try_release_backend_request(&memory,&life,&diagnostic)==0 && life.phase==BACKEND_REQUEST_RELEASED);
        assert(memory.header->outstanding[1]==0);
        assert(mark_backend_request_finished(&life,BACKEND_OUTCOME_CANCELLED)==1);
        assert(try_release_backend_request(&memory,&life,&diagnostic)==0 && memory.header->outstanding[1]==0);
        validate(&memory);assert(reset_backend_request_lifetime(&life)==0 && life.phase==BACKEND_REQUEST_IDLE);
    }
    assert(destroy_shared_backend_memory(&memory)==0);
}
static void test_busy_token_and_other_worker(void) {
    struct shared_backend_memory memory;create_memory(&memory);struct backend_state_diagnostic diagnostic;
    struct backend_request_lifetime life={0},other={0};begin(&memory,&life,0,0,1);begin(&memory,&other,1,1,2);
    assert(mark_backend_request_finished(&life,BACKEND_OUTCOME_COMPLETE)==0);
    struct backend_request_token original=life.token;
    int ready[2],release[2];assert(pipe(ready)==0 && pipe(release)==0);pid_t child=fork();assert(child>=0);
    if(!child){
        alarm(10);close(ready[0]);close(release[1]);
        if(try_lock_shared_backend_state(&memory,&diagnostic)<0)_exit(2);
        if(write(ready[1],"R",1)!=1)_exit(3);
        char byte;if(read(release[0],&byte,1)!=1)_exit(4);
        if(unlock_shared_backend_state(&memory)<0)_exit(5);
        _exit(0);
    }
    close(ready[1]);close(release[0]);char byte;assert(read(ready[0],&byte,1)==1);close(ready[0]);
    assert(try_release_backend_request(&memory,&life,&diagnostic)==1 && errno==EBUSY);
    assert(!memcmp(&life.token,&original,sizeof(original)) && life.phase==BACKEND_REQUEST_RELEASE_PENDING);
    assert(memory.header->outstanding[1]==1 && memory.header->outstanding[2]==1);
    assert(reset_backend_request_lifetime(&life)==-1 && errno==EBUSY);
    assert(write(release[1],"R",1)==1);close(release[1]);int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(try_release_backend_request(&memory,&life,&diagnostic)==0 && memory.header->outstanding[1]==0 && memory.header->outstanding[2]==1);
    assert(mark_backend_request_finished(&other,BACKEND_OUTCOME_CANCELLED)==0 && try_release_backend_request(&memory,&other,&diagnostic)==0);
    validate(&memory);assert(destroy_shared_backend_memory(&memory)==0);
}
static void test_client_output_lifetime(void) {
    struct shared_backend_memory memory;create_memory(&memory);struct backend_state_diagnostic diagnostic;
    struct parent_connection_ledger ledger={0};assert(create_parent_connection_ledger(2,2,1,&ledger)==0);
    assert(activate_parent_connection_worker(&ledger,0,1)==0);uint64_t connection;
    assert(register_parent_connection(&ledger,&connection)==0 && assign_parent_connection(&ledger,connection,0,1)==0);
    assert(finish_parent_connection_handoff(&ledger,connection)==0);
    struct backend_request_lifetime life={0};begin(&memory,&life,0,0,1);
    int client[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,client)==0);
    unsigned char output[]={0,'G','E','T','\r','\n',255};
    size_t unsent=sizeof(output); /* Verified final bytes already stored; client output not yet sent. */
    assert(mark_backend_request_finished(&life,BACKEND_OUTCOME_COMPLETE)==0);
    assert(try_release_backend_request(&memory,&life,&diagnostic)==0);
    assert(memory.header->outstanding[1]==0 && ledger.workers[0].outstanding==1 && unsent==sizeof(output));
    /* Normal write half-close still allows delivery: do not mark cancellation. */
    assert(shutdown(client[1],SHUT_WR)==0);char byte;assert(read(client[0],&byte,1)==0);
    assert(send(client[0],output,unsent,MSG_NOSIGNAL)==(ssize_t)unsent);unsent=0;
    unsigned char got[sizeof(output)];assert(read(client[1],got,sizeof(got))==sizeof(got) && !memcmp(got,output,sizeof(got)));
    assert(life.outcome==BACKEND_OUTCOME_COMPLETE && unsent==0);
    close(client[0]);close(client[1]);assert(apply_worker_connection_closed(&ledger,0,1,connection)==0 && ledger.total==0);
    destroy_parent_connection_ledger(&ledger);assert(destroy_shared_backend_memory(&memory)==0);
}
static void test_worker_death_stages(void) {
    for(unsigned int stage=0;stage<3;++stage){
        struct shared_backend_memory memory;create_memory(&memory);int ready[2];assert(pipe(ready)==0);
        pid_t child=fork();assert(child>=0);
        if(!child){
            alarm(10);close(ready[0]);struct backend_request_lifetime life={0};struct backend_state_diagnostic diagnostic;
            begin(&memory,&life,0,0,1);
            if(stage){if(mark_backend_request_finished(&life,BACKEND_OUTCOME_FAILED)<0)_exit(2);}
            if(stage==2 && try_release_backend_request(&memory,&life,&diagnostic)!=0)_exit(3);
            if(write(ready[1],"R",1)!=1)_exit(4);
            for(;;)pause();
        }
        close(ready[1]);char byte;assert(read(ready[0],&byte,1)==1);close(ready[0]);
        assert(memory.header->outstanding[1]==(stage==2?0:1));
        assert(kill(child,SIGKILL)==0);int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status));
        struct backend_state_diagnostic diagnostic;assert(try_lock_shared_backend_state(&memory,&diagnostic)==0);
        assert(reclaim_backend_worker_locked(&memory,0,1,&diagnostic)==0);
        assert(reclaim_backend_worker_locked(&memory,0,1,&diagnostic)==0);
        assert(validate_backend_reservations_locked(&memory,&diagnostic)==0 && memory.header->outstanding[1]==0);
        assert(memory.workers[1].state==BACKEND_WORKER_ACTIVE);assert(unlock_shared_backend_state(&memory)==0);
        assert(destroy_shared_backend_memory(&memory)==0);
    }
}
static void test_stale_and_fatal_tokens(void) {
    struct shared_backend_memory memory;create_memory(&memory);struct backend_state_diagnostic diagnostic;
    struct backend_request_lifetime old={0},current={0};begin(&memory,&old,0,0,1);
    assert(mark_backend_request_finished(&old,BACKEND_OUTCOME_COMPLETE)==0);
    assert(try_lock_shared_backend_state(&memory,&diagnostic)==0);
    assert(complete_backend_request_locked(&memory,&old.token,&diagnostic)==0);
    assert(begin_backend_request_locked(&memory,&current,0,1,0,2,0,&diagnostic)==0);
    assert(unlock_shared_backend_state(&memory)==0);
    assert(try_release_backend_request(&memory,&old,&diagnostic)==0 && memory.header->outstanding[2]==1);
    assert(mark_backend_request_finished(&current,BACKEND_OUTCOME_FAILED)==0);
    struct backend_request_token token=current.token;atomic_store(&memory.header->fatal,1);
    assert(try_release_backend_request(&memory,&current,&diagnostic)==-1);
    assert(current.phase==BACKEND_REQUEST_RELEASE_PENDING && !memcmp(&token,&current.token,sizeof(token)));
    assert(memory.header->outstanding[2]==1 && atomic_load(&memory.header->fatal));
    assert(destroy_shared_backend_memory(&memory)==0); /* All users stopped; do not reset untrusted state. */
}
int main(void) {
    alarm(25);setvbuf(stdout,NULL,_IOLBF,0);size_t before=fd_count();
    test_transitions_and_outcomes();puts("[PASS] lock-scoped reserve, COMPLETE/FAILED/CANCELLED, first outcome, duplicate release and reset guards");
    test_busy_token_and_other_worker();puts("[PASS] real process mutex EBUSY retains pending token and other-worker contribution, later release");
    test_client_output_lifetime();puts("[PASS] backend released before pending binary client output, worker count retained, half-close permits response");
    test_worker_death_stages();puts("[PASS] actual SIGKILL at ACTIVE/pending/released, parent generation reclaim exactly once");
    test_stale_and_fatal_tokens();puts("[PASS] stale token cannot release reused reservation; fatal retains pending evidence");
    assert(fd_count()==before);alarm(0);printf("backend lifetime: %zu bytes, no heap/FD/timer\n",sizeof(struct backend_request_lifetime));
    return 0;
}
