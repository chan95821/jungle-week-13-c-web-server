#define _GNU_SOURCE
#include "../../src/distribution_policy.h"
#include "../../src/backend_request_lifetime.h"
#include "../../src/parent_connection_ledger.h"
#include "../../src/worker_closed_channel.h"
#include "../../src/worker_fd_channel.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>
static enum backend_fault_step die_at;
static unsigned int die_occurrence=1,die_seen;
void shared_backend_fault_step(enum backend_fault_step step){if(step==die_at && ++die_seen==die_occurrence){raise(SIGKILL);_exit(99);}}
static struct distribution_observation observation(uint64_t us,uint64_t end,enum distribution_sample_result result){return(struct distribution_observation){us,end,result};}
static void test_math(void){
    struct distribution_timing timing={0};struct distribution_observation s=observation(800,2000,DISTRIBUTION_COMPLETED);
    assert(update_distribution_timing(&timing,&s)==0 && timing.representative_us==800);
    s=observation(1600,3000,DISTRIBUTION_COMPLETED);assert(update_distribution_timing(&timing,&s)==0 && timing.representative_us==900);
    s=observation(100,2000,DISTRIBUTION_COMPLETED);assert(update_distribution_timing(&timing,&s)==0 && timing.representative_us==800 && timing.last_finished_us==3000);
    s=observation(1,4000,DISTRIBUTION_CANCELLED);assert(update_distribution_timing(&timing,&s)==0 && timing.representative_us==800);
    timing=(struct distribution_timing){0};s=observation(1,4000,DISTRIBUTION_FAILED);
    assert(update_distribution_timing(&timing,&s)==0 && timing.representative_us==1000000);
    timing=(struct distribution_timing){0};s=observation(0,0,DISTRIBUTION_COMPLETED);
    assert(update_distribution_timing(&timing,&s)==0 && timing.representative_us==1);
    s=observation(UINT64_MAX,UINT64_MAX,DISTRIBUTION_COMPLETED);assert(update_distribution_timing(&timing,&s)==0);
    assert(timing.representative_us>1);s=observation(2,1,DISTRIBUTION_COMPLETED);
    assert(update_distribution_timing(&timing,&s)==-1 && errno==EINVAL);
    struct timespec a={1,999999500},b={2,1000};s=(struct distribution_observation){0};
    assert(distribution_elapsed_us(&a,&b,&s,DISTRIBUTION_COMPLETED)==0 && s.elapsed_us==1 && s.finished_us==2000001);
    assert(distribution_elapsed_us(&b,&a,&s,DISTRIBUTION_COMPLETED)==-1 && errno==ERANGE);
    b=(struct timespec){.tv_sec=(time_t)(UINT64_MAX/1000000+1)};
    assert(distribution_elapsed_us(&a,&b,&s,DISTRIBUTION_COMPLETED)==-1 && errno==EOVERFLOW);
    enum distribution_policy p;const char *names[]={"rr","least-outstanding","response-time","combined"};
    for(int i=0;i<4;++i){assert(parse_distribution_policy(names[i],&p)==0 && (int)p==i);}
    assert(parse_distribution_policy("auto",&p)==-1);
}
static size_t choose(enum distribution_policy p,struct distribution_candidate *c,struct distribution_turn *turn){
    struct distribution_turn next;size_t selected;assert(choose_distribution_candidate(p,c,2,turn,&selected,&next)==0);*turn=next;return selected;
}
static void test_choices(void){
    struct distribution_candidate c[2]={{.eligible=true},{.eligible=true}};struct distribution_turn turn={0};
    assert(choose(DISTRIBUTE_RR,c,&turn)==0 && choose(DISTRIBUTE_RR,c,&turn)==1);
    c[0].outstanding=5;c[1].outstanding=2;assert(choose(DISTRIBUTE_LEAST_OUTSTANDING,c,&turn)==1);
    c[0].outstanding=2;assert(choose(DISTRIBUTE_LEAST_OUTSTANDING,c,&turn)==0);
    c[0].timing=(struct distribution_timing){100,1000};c[1].timing=(struct distribution_timing){0};turn=(struct distribution_turn){0};
    assert(choose(DISTRIBUTE_RESPONSE_TIME,c,&turn)==1); /* Unknown beats known. */
    c[1].timing=(struct distribution_timing){500,1000};turn=(struct distribution_turn){0};
    for(size_t i=0;i<31;++i)assert(choose(DISTRIBUTE_RESPONSE_TIME,c,&turn)==0);
    assert(turn.selections==31 && choose(DISTRIBUTE_RESPONSE_TIME,c,&turn)==1 && turn.selections==0);
    assert(choose(DISTRIBUTE_RESPONSE_TIME,c,&turn)==0);
    c[1].timing.representative_us=100;c[0].outstanding=4;c[1].outstanding=0;turn=(struct distribution_turn){0};
    assert(choose(DISTRIBUTE_COMBINED,c,&turn)==1);
    c[0].outstanding=c[1].outstanding=UINT64_MAX;turn=(struct distribution_turn){0};
    assert(choose(DISTRIBUTE_COMBINED,c,&turn)==0 && choose(DISTRIBUTE_COMBINED,c,&turn)==1);
    c[0].eligible=false;assert(choose(DISTRIBUTE_RR,c,&turn)==1);c[1].eligible=false;
    size_t untouched=99;struct distribution_turn next={99,99};
    assert(choose_distribution_candidate(DISTRIBUTE_RR,c,2,&turn,&untouched,&next)==-1 && errno==EAGAIN && untouched==99 && next.cursor==99);
}
static void create_shared(struct shared_backend_memory *memory){
    *memory=(struct shared_backend_memory){0};assert(create_shared_backend_memory(2,8,8192,memory)==0);
    assert(try_lock_shared_backend_state(memory,NULL)==0);
    for(uint32_t i=0;i<2;++i){uint64_t gen;assert(activate_backend_worker_locked(memory,i,&gen,NULL)==0);}
    assert(unlock_shared_backend_state(memory)==0);
}
static void wait_killed(pid_t child){int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);}
static bool committed(enum backend_fault_step step){return step==BACKEND_FAULT_COMMITTED || step==BACKEND_FAULT_IDLE;}
static void test_stats_crash_transactions(void){
    const enum backend_fault_step steps[]={BACKEND_FAULT_JOURNAL_FILLED,BACKEND_FAULT_PREPARED,
        BACKEND_FAULT_RESERVATION_WRITTEN,BACKEND_FAULT_WORKER_WRITTEN,BACKEND_FAULT_TOTAL_WRITTEN,
        BACKEND_FAULT_TIMING_REPRESENTATIVE,BACKEND_FAULT_TIMING_WRITTEN,BACKEND_FAULT_TURN_WRITTEN,BACKEND_FAULT_COMMITTED,BACKEND_FAULT_IDLE};
    for(size_t i=0;i<sizeof(steps)/sizeof(steps[0]);++i){
        struct shared_backend_memory memory;create_shared(&memory);struct backend_request_token token;
        assert(try_lock_shared_backend_state(&memory,NULL)==0);
        memory.header->timing[1]=(struct distribution_timing){800,2000};
        assert(reserve_backend_request_locked(&memory,0,1,0,1,0,&token,NULL)==0);assert(unlock_shared_backend_state(&memory)==0);
        pid_t child=fork();assert(child>=0);if(!child){alarm(5);die_at=steps[i];assert(try_lock_shared_backend_state(&memory,NULL)==0);
            struct distribution_observation s=observation(1600,3000,DISTRIBUTION_COMPLETED);
            complete_backend_request_observed_locked(&memory,&token,&s,NULL);_exit(7);}
        wait_killed(child);assert(try_lock_shared_backend_state(&memory,NULL)==0);
        assert(memory.header->timing[1].representative_us==(committed(steps[i])?900:800));
        assert(memory.header->outstanding[1]==(committed(steps[i])?0:1));
        struct distribution_observation s=observation(1600,3000,DISTRIBUTION_COMPLETED);
        int result=complete_backend_request_observed_locked(&memory,&token,&s,NULL);
        assert(result==(committed(steps[i])?1:0) && memory.header->timing[1].representative_us==900);
        assert(complete_backend_request_observed_locked(&memory,&token,&s,NULL)==1 && memory.header->timing[1].representative_us==900);
        assert(unlock_shared_backend_state(&memory)==0);assert(destroy_shared_backend_memory(&memory)==0);
    }
}
static void test_selection_crash_transactions(void){
    const enum backend_fault_step steps[]={BACKEND_FAULT_PREPARED,BACKEND_FAULT_RESERVATION_WRITTEN,
        BACKEND_FAULT_WORKER_WRITTEN,BACKEND_FAULT_TOTAL_WRITTEN,BACKEND_FAULT_QUERY_CURSOR_WRITTEN,BACKEND_FAULT_TURN_WRITTEN,
        BACKEND_FAULT_COMMITTED,BACKEND_FAULT_IDLE};
    for(size_t i=0;i<sizeof(steps)/sizeof(steps[0]);++i){
        struct shared_backend_memory memory;create_shared(&memory);
        pid_t child=fork();assert(child>=0);if(!child){alarm(5);die_at=steps[i];assert(try_lock_shared_backend_state(&memory,NULL)==0);
            struct backend_request_token token;uint32_t backend;select_and_reserve_query_locked(&memory,DISTRIBUTE_RESPONSE_TIME,0,1,0,&token,&backend,NULL);_exit(7);}
        wait_killed(child);assert(try_lock_shared_backend_state(&memory,NULL)==0);
        assert(memory.header->query_selections==(committed(steps[i])?1:0));
        assert(memory.header->next_query_turn==(committed(steps[i])?1:0));
        assert(memory.header->outstanding[1]==(committed(steps[i])?1:0));
        assert(reclaim_backend_worker_locked(&memory,0,1,NULL)==0);assert(unlock_shared_backend_state(&memory)==0);assert(destroy_shared_backend_memory(&memory)==0);
    }
}
static void test_recovery_interruption(void){
    struct shared_backend_memory memory;create_shared(&memory);struct backend_request_token token;
    assert(try_lock_shared_backend_state(&memory,NULL)==0);memory.header->timing[1]=(struct distribution_timing){800,2000};
    assert(reserve_backend_request_locked(&memory,0,1,0,1,0,&token,NULL)==0);assert(unlock_shared_backend_state(&memory)==0);
    pid_t child=fork();assert(child>=0);if(!child){die_at=BACKEND_FAULT_TIMING_WRITTEN;assert(try_lock_shared_backend_state(&memory,NULL)==0);
        struct distribution_observation s=observation(1600,3000,DISTRIBUTION_COMPLETED);complete_backend_request_observed_locked(&memory,&token,&s,NULL);_exit(7);}
    wait_killed(child);
    child=fork();assert(child>=0);if(!child){die_at=BACKEND_FAULT_RESTORE_TIMING_REPRESENTATIVE;die_occurrence=2;try_lock_shared_backend_state(&memory,NULL);_exit(7);}
    wait_killed(child);assert(try_lock_shared_backend_state(&memory,NULL)==0);
    assert(memory.header->timing[1].representative_us==800 && memory.header->timing[1].last_finished_us==2000 && memory.header->outstanding[1]==1);
    assert(reclaim_backend_worker_locked(&memory,0,1,NULL)==0 && memory.header->timing[1].representative_us==800);
    assert(unlock_shared_backend_state(&memory)==0);assert(destroy_shared_backend_memory(&memory)==0);
}
static void test_shared_policy_contention(void){
    struct shared_backend_memory memory;create_shared(&memory);pid_t children[2];
    for(uint32_t worker=0;worker<2;++worker){
        children[worker]=fork();assert(children[worker]>=0);
        if(!children[worker]){
            alarm(10);
            for(size_t request=0;request<16;++request){
                while(try_lock_shared_backend_state(&memory,NULL)<0){if(errno!=EBUSY)_exit(2);sched_yield();}
                struct backend_request_token token;uint32_t backend;
                if(select_and_reserve_query_locked(&memory,DISTRIBUTE_COMBINED,worker,1,worker,&token,&backend,NULL)<0)_exit(3);
                if(unlock_shared_backend_state(&memory)<0)_exit(4);
                struct distribution_observation sample=observation(800,2000,DISTRIBUTION_COMPLETED);
                while(try_lock_shared_backend_state(&memory,NULL)<0){if(errno!=EBUSY)_exit(5);sched_yield();}
                if(complete_backend_request_observed_locked(&memory,&token,&sample,NULL)!=0)_exit(6);
                if(unlock_shared_backend_state(&memory)<0)_exit(7);
            }
            _exit(0);
        }
    }
    for(size_t i=0;i<2;++i){int status;assert(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && WEXITSTATUS(status)==0);}
    assert(try_lock_shared_backend_state(&memory,NULL)==0);
    assert(validate_backend_reservations_locked(&memory,NULL)==0 && memory.header->query_selections==0);
    assert(memory.header->timing[1].representative_us==800 && memory.header->timing[2].representative_us==800);
    assert(!memory.header->outstanding[1] && !memory.header->outstanding[2]);
    assert(unlock_shared_backend_state(&memory)==0);assert(destroy_shared_backend_memory(&memory)==0);
}
static void test_submission_preserves_query_exploration(void){
    struct shared_backend_memory memory;create_shared(&memory);
    assert(try_lock_shared_backend_state(&memory,NULL)==0);
    struct backend_request_token query,submission;uint32_t selected;
    assert(select_and_reserve_query_locked(&memory,DISTRIBUTE_RESPONSE_TIME,0,1,0,&query,&selected,NULL)==0 && selected==1);
    assert(memory.header->query_selections==1 && memory.header->next_query_turn==1);
    assert(reserve_backend_request_locked(&memory,1,1,1,0,memory.header->next_query_turn,&submission,NULL)==0);
    assert(memory.header->query_selections==1 && memory.header->next_query_turn==1);
    assert(complete_backend_request_locked(&memory,&submission,NULL)==0);
    assert(complete_backend_request_locked(&memory,&query,NULL)==0);
    assert(unlock_shared_backend_state(&memory)==0 && destroy_shared_backend_memory(&memory)==0);
}
static void test_worker_observed_frame(void){
    struct parent_connection_ledger ledger={0};assert(create_parent_connection_ledger(4,2,2,&ledger)==0);
    assert(activate_parent_connection_worker(&ledger,0,1)==0 && activate_parent_connection_worker(&ledger,1,1)==0);
    bool available[]={true,true};uint64_t id;uint32_t worker;
    assert(register_parent_connection(&ledger,&id)==0);
    assert(select_and_assign_parent_connection(&ledger,id,DISTRIBUTE_RR,available,&worker)==0 && worker==0);
    int channel[2];assert(create_worker_fd_channel(channel)==0);struct worker_closed_queue queue={0};assert(create_worker_closed_queue(2,&queue)==0);
    struct worker_connection_closed sent={.worker_index=0,.worker_generation=1,.connection_id=id,.observation={800,2000,DISTRIBUTION_COMPLETED}};
    assert(enqueue_worker_connection_closed(&queue,&sent)==0 && advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_READY);
    struct worker_closed_receiver receiver={0};struct worker_connection_closed received={0};
    assert(advance_worker_closed_receive(channel[0],0,1,&receiver,&received)==WORKER_CLOSED_READY);
    assert(received.observation.elapsed_us==800 && received.observation.result==DISTRIBUTION_COMPLETED);
    assert(apply_worker_connection_observed_closed(&ledger,0,1,id,&received.observation)==0 && ledger.workers[0].timing.representative_us==0);
    received.observation.elapsed_us=1600;
    assert(apply_worker_connection_observed_closed(&ledger,0,1,id,&received.observation)==1);
    assert(finish_parent_connection_handoff(&ledger,id)==0 && ledger.workers[0].timing.representative_us==800);
    assert(apply_worker_connection_observed_closed(&ledger,0,1,id,&received.observation)==1 && ledger.workers[0].timing.representative_us==800);
    assert(register_parent_connection(&ledger,&id)==0);
    assert(select_and_assign_parent_connection(&ledger,id,DISTRIBUTE_RESPONSE_TIME,available,&worker)==0 && worker==1);
    assert(reclaim_reaped_worker_connections(&ledger,1,1)==0 && ledger.workers[1].timing.representative_us==0);
    close(channel[0]);close(channel[1]);destroy_worker_closed_queue(&queue);destroy_parent_connection_ledger(&ledger);
}
static void test_malformed_observed_frames(void){
    const size_t offsets[]={4,32,40,48,52};
    for(size_t i=0;i<5;++i){
        int channel[2];assert(create_worker_fd_channel(channel)==0);
        struct worker_closed_queue queue={0};assert(create_worker_closed_queue(1,&queue)==0);
        struct worker_connection_closed sent={.worker_index=0,.worker_generation=1,.connection_id=(UINT64_C(1)<<32)|1,
            .observation={800,2000,DISTRIBUTION_COMPLETED}};
        assert(enqueue_worker_connection_closed(&queue,&sent)==0 && advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_READY);
        unsigned char wire[WORKER_CLOSED_FRAME_BYTES];assert(recv(channel[0],wire,sizeof(wire),0)==sizeof(wire));
        if(offsets[i]==4)wire[7]=1;
        else if(offsets[i]==40)memset(wire+40,0,8);
        else wire[offsets[i]]=255;
        assert(send(channel[1],wire,sizeof(wire),0)==sizeof(wire));
        struct worker_closed_receiver receiver={0};struct worker_connection_closed out={.worker_index=99};
        assert(advance_worker_closed_receive(channel[0],0,1,&receiver,&out)==-1 && errno==EPROTO && out.worker_index==99);
        close(channel[0]);close(channel[1]);destroy_worker_closed_queue(&queue);
    }
}
static void test_clock_lifetime(void){
    struct shared_backend_memory memory;create_shared(&memory);struct backend_request_lifetime life={0};
    assert(try_lock_shared_backend_state(&memory,NULL)==0);uint32_t chosen;assert(begin_selected_query_request_locked(&memory,&life,DISTRIBUTE_RESPONSE_TIME,0,1,0,&chosen,NULL)==0 && chosen==1);assert(unlock_shared_backend_state(&memory)==0);
    struct timespec start={1,0},end={1,800000};assert(start_backend_request_clock(&life,&start)==0);
    assert(finish_backend_request_clock(&life,&end,BACKEND_OUTCOME_COMPLETE)==0 && life.observation.elapsed_us==800);
    end.tv_sec=2;assert(finish_backend_request_clock(&life,&end,BACKEND_OUTCOME_FAILED)==1 && life.observation.elapsed_us==800);
    assert(try_release_backend_request(&memory,&life,NULL)==0 && memory.header->timing[1].representative_us==800);
    assert(reset_backend_request_lifetime(&life)==0);
    assert(try_lock_shared_backend_state(&memory,NULL)==0);assert(begin_backend_request_locked(&memory,&life,0,1,0,1,0,NULL)==0);assert(unlock_shared_backend_state(&memory)==0);
    assert(start_backend_request_clock(&life,&start)==0);end=(struct timespec){0};
    assert(finish_backend_request_clock(&life,&end,BACKEND_OUTCOME_COMPLETE)==-1 && errno==ERANGE);
    assert(life.phase==BACKEND_REQUEST_RELEASE_PENDING && try_release_backend_request(&memory,&life,NULL)==0);
    assert(memory.header->timing[1].representative_us==800);assert(destroy_shared_backend_memory(&memory)==0);
}
int main(void){
    alarm(40);setvbuf(stdout,NULL,_IOLBF,0);
    test_math();test_choices();puts("[PASS] EWMA/time/overflow/failure rules; four policies, unknown, tie RR and 32nd exploration");
    test_stats_crash_transactions();test_selection_crash_transactions();test_recovery_interruption();
    puts("[PASS] actual SIGKILL: timing+release 10, selection+reservation 8, recovery interrupted and recovered again");
    test_shared_policy_contention();puts("[PASS] two-process policy selection+reservation and observed completion contention");
    test_submission_preserves_query_exploration();
    test_worker_observed_frame();test_malformed_observed_frames();test_clock_lifetime();puts("[PASS] CLOSED v2 timing, early/duplicate sample exactly once, worker selection, backend frozen interval");
    printf("policy storage: shared-header=%zu journal=%zu worker=%zu entry=%zu CLOSED=%d token=%zu lifetime=%zu\n",sizeof(struct shared_backend_header),sizeof(struct backend_recovery_record),sizeof(struct parent_connection_worker),sizeof(struct parent_connection_entry),WORKER_CLOSED_FRAME_BYTES,sizeof(struct worker_connection_closed),sizeof(struct backend_request_lifetime));
    alarm(0);return 0;
}
