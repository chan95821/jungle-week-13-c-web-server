#define _GNU_SOURCE
#include "../../src/parent_connection_ledger.h"
#include "../../src/concurrency_limits.h"
#include "../../src/worker_closed_channel.h"
#include "../../src/worker_fd_channel.h"
#include "../../src/parent_client_queue.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t send_limit,receive_limit;
static int send_error,receive_error;
ssize_t closed_test_send(int fd,const void *buffer,size_t length,int flags) {
    if (send_error) { errno=send_error; send_error=0; return -1; }
    if (send_limit && length>send_limit) length=send_limit;
    return send(fd,buffer,length,flags);
}
ssize_t closed_test_recvmsg(int fd,struct msghdr *message,int flags) {
    if (receive_error) { errno=receive_error; receive_error=0; return -1; }
    if (receive_limit && message->msg_iov[0].iov_len>receive_limit) message->msg_iov[0].iov_len=receive_limit;
    return recvmsg(fd,message,flags);
}
static size_t fd_count(void) {
    DIR *dir=opendir("/proc/self/fd"); assert(dir);
    size_t count=0; struct dirent *entry;
    while ((entry=readdir(dir))) if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
    assert(closedir(dir)==0); return count;
}
static struct worker_connection_closed notice(uint32_t worker,uint64_t generation,uint64_t id) {
    return (struct worker_connection_closed){.worker_index=worker,.worker_generation=generation,.connection_id=id};
}
static void check(struct parent_connection_ledger *ledger,size_t total,size_t queued) {
    assert(validate_parent_connection_ledger(ledger)==0);
    assert(ledger->total==total && ledger->queued==queued);
}
static void test_accounting(void) {
    struct parent_connection_ledger ledger={0};
    assert(create_parent_connection_ledger(3,2,1,&ledger)==0);
    assert(activate_parent_connection_worker(&ledger,0,7)==0);
    assert(activate_parent_connection_worker(&ledger,1,9)==0);
    uint64_t a,b,c,sentinel=99;
    assert(register_parent_connection(&ledger,&a)==0);
    assert(register_parent_connection(&ledger,&b)==0);
    assert(register_parent_connection(&ledger,&c)==0); check(&ledger,3,3);
    assert(register_parent_connection(&ledger,&sentinel)==-1 && errno==ENOSPC && sentinel==99);
    assert(assign_parent_connection(&ledger,a,0,7)==0);
    assert(assign_parent_connection(&ledger,b,0,7)==-1 && errno==EAGAIN);
    assert(assign_parent_connection(&ledger,b,1,9)==0); check(&ledger,3,1);
    assert(apply_worker_connection_closed(&ledger,1,9,a)==-1 && errno==EPROTO);
    assert(apply_worker_connection_closed(&ledger,0,8,a)==-1 && errno==EPROTO);
    assert(apply_worker_connection_closed(&ledger,0,7,a+(UINT64_C(1)<<32))==-1 && errno==EPROTO);
    assert(apply_worker_connection_closed(&ledger,0,7,a)==0); check(&ledger,3,1);
    assert(apply_worker_connection_closed(&ledger,0,7,a)==1); check(&ledger,3,1);
    assert(finish_parent_connection_handoff(&ledger,a)==0); check(&ledger,2,1);
    assert(apply_worker_connection_closed(&ledger,0,7,a)==1);
    uint64_t newer; assert(register_parent_connection(&ledger,&newer)==0 && newer>a);
    assert(apply_worker_connection_closed(&ledger,0,7,a)==1); check(&ledger,3,2);
    assert(finish_parent_connection_handoff(&ledger,b)==0);
    assert(reclaim_reaped_worker_connections(&ledger,0,7)==0); check(&ledger,3,2);
    assert(ledger.workers[1].outstanding==1);
    assert(reclaim_reaped_worker_connections(&ledger,0,7)==1);
    assert(activate_parent_connection_worker(&ledger,0,8)==0);
    assert(apply_worker_connection_closed(&ledger,0,7,newer)==1);
    assert(assign_parent_connection(&ledger,newer,0,8)==0);
    assert(apply_worker_connection_closed(&ledger,0,7,newer)==1); check(&ledger,3,1);
    assert(reclaim_reaped_worker_connections(&ledger,0,8)==0); check(&ledger,2,1);
    assert(apply_worker_connection_closed(&ledger,1,9,b)==0); check(&ledger,1,1);
    assert(cancel_queued_parent_connection(&ledger,c)==0); check(&ledger,0,0);
    ledger.entries[0].connection_id=(UINT64_C(0xffffffff)<<32)|1;
    assert(register_parent_connection(&ledger,&sentinel)==-1 && errno==EOVERFLOW); check(&ledger,0,0);
    destroy_parent_connection_ledger(&ledger); destroy_parent_connection_ledger(&ledger);
}
static void test_selected_capacity_bounds(void) {
    struct parent_connection_ledger ledger={0};
    assert(create_parent_connection_ledger(INITIAL_TOTAL_CONNECTION_LIMIT,INITIAL_WORKER_COUNT,
                                           INITIAL_WORKER_CONNECTION_LIMIT,&ledger)==0);
    uint64_t ids[INITIAL_TOTAL_CONNECTION_LIMIT];
    for(uint32_t worker=0;worker<INITIAL_WORKER_COUNT;++worker)
        assert(activate_parent_connection_worker(&ledger,worker,1)==0);
    for(size_t i=0;i<INITIAL_TOTAL_CONNECTION_LIMIT;++i) assert(register_parent_connection(&ledger,&ids[i])==0);
    check(&ledger,1152,1152);
    uint64_t unchanged=99;assert(register_parent_connection(&ledger,&unchanged)==-1 && errno==ENOSPC && unchanged==99);
    for(uint32_t worker=0;worker<INITIAL_WORKER_COUNT;++worker)
        for(size_t i=0;i<INITIAL_WORKER_CONNECTION_LIMIT;++i)
            assert(assign_parent_connection(&ledger,ids[worker*INITIAL_WORKER_CONNECTION_LIMIT+i],worker,1)==0);
    check(&ledger,1152,128);
    assert(assign_parent_connection(&ledger,ids[1024],0,1)==-1 && errno==EAGAIN);
    struct worker_closed_queue queue={0};assert(create_worker_closed_queue(INITIAL_WORKER_CONNECTION_LIMIT,&queue)==0);
    for(size_t i=0;i<INITIAL_WORKER_CONNECTION_LIMIT;++i) {
        struct worker_connection_closed closed=notice(0,1,ids[i]);
        assert(enqueue_worker_connection_closed(&queue,&closed)==0);
        assert(apply_worker_connection_closed(&ledger,0,1,ids[i])==0);
    }
    assert(queue.count==256 && !worker_closed_has_capacity(&queue));check(&ledger,1152,128);
    for(size_t i=0;i<INITIAL_WORKER_CONNECTION_LIMIT;++i) assert(finish_parent_connection_handoff(&ledger,ids[i])==0);
    check(&ledger,896,128);
    for(uint32_t worker=0;worker<INITIAL_WORKER_COUNT;++worker) assert(reclaim_reaped_worker_connections(&ledger,worker,1)==0);
    check(&ledger,128,128);
    for(size_t i=1024;i<1152;++i) assert(cancel_queued_parent_connection(&ledger,ids[i])==0);
    check(&ledger,0,0);destroy_worker_closed_queue(&queue);destroy_parent_connection_ledger(&ledger);
}

static void test_partial_and_full_queue(void) {
    int channel[2]; assert(create_worker_fd_channel(channel)==0);
    struct worker_closed_queue queue={0}; assert(create_worker_closed_queue(2,&queue)==0);
    struct worker_connection_closed a=notice(0,7,(UINT64_C(1)<<32)|1),b=notice(0,7,(UINT64_C(1)<<32)|2);
    assert(enqueue_worker_connection_closed(&queue,&a)==0);
    assert(enqueue_worker_connection_closed(&queue,&a)==1 && queue.count==1);
    assert(enqueue_worker_connection_closed(&queue,&b)==0 && !worker_closed_has_capacity(&queue));
    struct worker_connection_closed c=notice(0,7,(UINT64_C(1)<<32)|3);
    assert(enqueue_worker_connection_closed(&queue,&c)==-1 && errno==ENOSPC);
    send_error=EINTR; assert(advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_PENDING && queue.sent_bytes==0);
    send_error=EAGAIN; assert(advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_PENDING);
    send_limit=3; receive_limit=2;
    struct worker_closed_receiver receiver={0}; struct worker_connection_closed out=notice(9,99,99);
    receive_error=EINTR; assert(advance_worker_closed_receive(channel[0],0,7,&receiver,&out)==WORKER_CLOSED_PENDING);
    assert(advance_worker_closed_receive(channel[0],0,7,&receiver,&out)==WORKER_CLOSED_PENDING && out.worker_index==9);
    for (size_t i=0;queue.count && i<100;++i) {
        int result=advance_worker_closed_send(channel[1],&queue);
        assert(result==WORKER_CLOSED_PENDING || result==WORKER_CLOSED_READY);
        if (queue.count==1) break;
    }
    assert(queue.count==1 && worker_closed_has_capacity(&queue));
    assert(enqueue_worker_connection_closed(&queue,&c)==0); /* Ring wraps after partial head. */
    send_limit=0;
    while (queue.count) assert(advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_READY);
    const uint64_t expected[]={a.connection_id,b.connection_id,c.connection_id};
    for (size_t j=0;j<3;++j) {
        int result=0;
        for(size_t i=0;i<64 && result!=WORKER_CLOSED_READY;++i)
            result=advance_worker_closed_receive(channel[0],0,7,&receiver,&out);
        assert(result==WORKER_CLOSED_READY && out.connection_id==expected[j]);
    }
    receive_limit=0;
    /* Fill actual kernel buffer; retain one pending CLOSED until room returns. */
    unsigned char junk[1024]={0}; size_t filled=0;
    for (;;) { ssize_t n=send(channel[1],junk,sizeof(junk),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n<0){assert(errno==EAGAIN || errno==EWOULDBLOCK);break;} filled+=(size_t)n; }
    assert(filled && enqueue_worker_connection_closed(&queue,&a)==0);
    assert(advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_PENDING && queue.count==1);
    while (filled) { ssize_t n=recv(channel[0],junk,filled<sizeof(junk)?filled:sizeof(junk),MSG_DONTWAIT); assert(n>0); filled-=(size_t)n; }
    assert(advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_READY);
    assert(advance_worker_closed_receive(channel[0],0,7,&receiver,&out)==WORKER_CLOSED_READY);
    close(channel[0]);
    assert(enqueue_worker_connection_closed(&queue,&b)==0);
    assert(advance_worker_closed_send(channel[1],&queue)==-1 && queue.failed);
    assert(!worker_closed_has_capacity(&queue));
    close(channel[1]); destroy_worker_closed_queue(&queue); destroy_worker_closed_queue(&queue);
}
static void valid_wire(unsigned char frame[WORKER_CLOSED_FRAME_BYTES]) {
    int channel[2]; assert(create_worker_fd_channel(channel)==0);
    struct worker_closed_queue queue={0}; assert(create_worker_closed_queue(1,&queue)==0);
    struct worker_connection_closed a=notice(0,7,(UINT64_C(1)<<32)|1);
    assert(enqueue_worker_connection_closed(&queue,&a)==0);
    assert(advance_worker_closed_send(channel[1],&queue)==WORKER_CLOSED_READY);
    assert(recv(channel[0],frame,WORKER_CLOSED_FRAME_BYTES,0)==WORKER_CLOSED_FRAME_BYTES);
    close(channel[0]);close(channel[1]);destroy_worker_closed_queue(&queue);
}
static void test_bad_frames_and_ancillary(void) {
    unsigned char original[WORKER_CLOSED_FRAME_BYTES]; valid_wire(original);
    const size_t offsets[]={0,4,8,12,23,24,31};
    for(size_t i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
        int channel[2]; assert(create_worker_fd_channel(channel)==0);
        unsigned char bad[WORKER_CLOSED_FRAME_BYTES]; memcpy(bad,original,sizeof(bad));
        if(offsets[i]==24) memset(bad+24,0,8);
        else if(offsets[i]==31) memset(bad+24,0,4);
        else bad[offsets[i]]^=1;
        assert(send(channel[1],bad,sizeof(bad),0)==sizeof(bad));
        struct worker_closed_receiver receiver={0}; struct worker_connection_closed out=notice(99,99,99);
        assert(advance_worker_closed_receive(channel[0],0,7,&receiver,&out)==-1 && errno==EPROTO);
        assert(receiver.failed && out.worker_index==99);
        close(channel[0]);close(channel[1]);
    }
    for(size_t count=1;count<=32;count*=32) {
        size_t before=fd_count(); int channel[2]; assert(create_worker_fd_channel(channel)==0);
        int rights[32]; for(size_t i=0;i<count;++i) rights[i]=channel[0];
        union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(32*sizeof(int))]; } control={0};
        struct iovec vector={original,sizeof(original)};
        struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=CMSG_SPACE(count*sizeof(int))};
        struct cmsghdr *header=CMSG_FIRSTHDR(&message); header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
        header->cmsg_len=CMSG_LEN(count*sizeof(int));memcpy(CMSG_DATA(header),rights,count*sizeof(int));
        assert(sendmsg(channel[1],&message,0)==sizeof(original));
        struct worker_closed_receiver receiver={0};struct worker_connection_closed out={0};
        assert(advance_worker_closed_receive(channel[0],0,7,&receiver,&out)==-1 && errno==EPROTO);
        close(channel[0]);close(channel[1]); assert(fd_count()==before);
    }
    const size_t lengths[]={0,1,WORKER_CLOSED_FRAME_BYTES-1};
    for(size_t i=0;i<3;++i) {
        int channel[2];assert(create_worker_fd_channel(channel)==0);
        if(lengths[i]) assert(send(channel[1],original,lengths[i],0)==(ssize_t)lengths[i]);
        close(channel[1]);struct worker_closed_receiver receiver={0};struct worker_connection_closed out={0};
        int result=advance_worker_closed_receive(channel[0],0,7,&receiver,&out);
        if(lengths[i]) {assert(result==WORKER_CLOSED_PENDING);result=advance_worker_closed_receive(channel[0],0,7,&receiver,&out);assert(result==-1 && errno==EPROTO);}
        else assert(result==WORKER_CLOSED_EOF || (result==-1 && errno==ECONNRESET));
        close(channel[0]);
    }
}
static void test_fifo_fd_completion_flow(void) {
    struct parent_connection_ledger ledger={0};struct parent_client_queue fifo={0};
    assert(create_parent_connection_ledger(2,1,1,&ledger)==0);
    assert(activate_parent_connection_worker(&ledger,0,7)==0);
    assert(create_parent_client_queue(2,&fifo)==0);
    int peers[2],channel[2];assert(create_worker_fd_channel(channel)==0);
    struct timespec accepted={.tv_sec=1};
    for(size_t i=0;i<2;++i) {
        int client[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,client)==0);peers[i]=client[1];
        uint64_t id;assert(register_parent_connection(&ledger,&id)==0);
        assert(enqueue_parent_client(&fifo,&client[0],id,&accepted)==0 && client[0]==-1);
    }
    assert(!parent_accept_has_capacity(&fifo,ledger.total,2,false));
    struct parent_queued_client queued={.client_fd=-1};assert(dequeue_parent_client(&fifo,&queued)==1);
    assert(assign_parent_connection(&ledger,queued.connection_id,0,7)==0);
    struct worker_client_metadata meta={0,7,queued.connection_id,queued.accepted_at};
    struct worker_fd_sender sender={0};assert(prepare_worker_fd_transfer(&sender,&queued.client_fd,&meta)==0);
    assert(advance_worker_fd_send(channel[0],&sender)==WORKER_FD_READY);
    struct worker_fd_receiver fd_receiver={0};struct worker_received_client received={.client_fd=-1};
    int result=0;for(size_t i=0;i<4 && result!=WORKER_FD_READY;++i) result=advance_worker_fd_receive(channel[1],0,7,&fd_receiver,&received);
    assert(result==WORKER_FD_READY);close(received.client_fd);
    struct worker_closed_queue completed={0};assert(create_worker_closed_queue(1,&completed)==0);
    struct worker_connection_closed closed=notice(0,7,received.metadata.connection_id);
    assert(enqueue_worker_connection_closed(&completed,&closed)==0 && !worker_closed_has_capacity(&completed));
    assert(advance_worker_closed_send(channel[1],&completed)==WORKER_CLOSED_READY);
    struct worker_closed_receiver receiver={0};struct worker_connection_closed out={0};
    assert(advance_worker_closed_receive(channel[0],0,7,&receiver,&out)==WORKER_CLOSED_READY);
    assert(apply_worker_connection_closed(&ledger,0,7,out.connection_id)==0);check(&ledger,2,1);
    assert(!parent_accept_has_capacity(&fifo,ledger.total,2,false));
    assert(finish_parent_connection_handoff(&ledger,out.connection_id)==0);check(&ledger,1,1);
    assert(parent_accept_has_capacity(&fifo,ledger.total,2,false));
    assert(!parent_accept_has_capacity(&fifo,ledger.total,2,true));
    assert(dequeue_parent_client(&fifo,&queued)==1);close(queued.client_fd);
    assert(cancel_queued_parent_connection(&ledger,queued.connection_id)==0);check(&ledger,0,0);
    char byte;assert(read(peers[0],&byte,1)==0 && read(peers[1],&byte,1)==0);
    close(peers[0]);close(peers[1]);close(channel[0]);close(channel[1]);
    destroy_worker_closed_queue(&completed);assert(destroy_parent_client_queue(&fifo)==0);destroy_parent_connection_ledger(&ledger);
}
static void test_reaped_worker_boundaries(void) {
    for(unsigned int stage=0;stage<3;++stage) {
        struct parent_connection_ledger ledger={0};assert(create_parent_connection_ledger(2,2,1,&ledger)==0);
        assert(activate_parent_connection_worker(&ledger,0,7)==0 && activate_parent_connection_worker(&ledger,1,9)==0);
        uint64_t id,other;assert(register_parent_connection(&ledger,&id)==0 && register_parent_connection(&ledger,&other)==0);
        assert(assign_parent_connection(&ledger,id,0,7)==0 && assign_parent_connection(&ledger,other,1,9)==0);
        int channel[2],client[2],sync[2];assert(create_worker_fd_channel(channel)==0);
        assert(socketpair(AF_UNIX,SOCK_STREAM,0,client)==0 && pipe(sync)==0);
        pid_t child=fork();assert(child>=0);
        if(child==0) {
            close(channel[0]);close(client[0]);close(client[1]);close(sync[0]);
            if(stage) {
                struct worker_fd_receiver rx={0};struct worker_received_client got={.client_fd=-1};int result=0;
                while(result!=WORKER_FD_READY) {result=advance_worker_fd_receive(channel[1],0,7,&rx,&got);if(result<0)_exit(3);}
                close(got.client_fd);
                struct worker_closed_queue queue={0};if(create_worker_closed_queue(1,&queue)<0)_exit(4);
                struct worker_connection_closed closed=notice(0,7,id);if(enqueue_worker_connection_closed(&queue,&closed)<0)_exit(5);
                send_limit=stage==1?1:0;
                int result_send=advance_worker_closed_send(channel[1],&queue);
                if(result_send!=(stage==1?WORKER_CLOSED_PENDING:WORKER_CLOSED_READY))_exit(6);
            }
            if(write(sync[1],"R",1)!=1)_exit(7);
            for(;;) pause();
        }
        close(channel[1]);close(sync[1]);
        struct worker_fd_sender sender={0};struct worker_client_metadata meta={0,7,id,{.tv_sec=1}};
        assert(prepare_worker_fd_transfer(&sender,&client[0],&meta)==0);
        assert(advance_worker_fd_send(channel[0],&sender)==WORKER_FD_READY);
        char marker;assert(read(sync[0],&marker,1)==1);close(sync[0]);
        assert(kill(child,SIGKILL)==0);int status;assert(waitpid(child,&status,0)==child && WIFSIGNALED(status));
        abort_worker_fd_send(&sender);
        struct worker_closed_receiver receiver={0};struct worker_connection_closed out={0};
        int result=advance_worker_closed_receive(channel[0],0,7,&receiver,&out);
        if(stage==2) {assert(result==WORKER_CLOSED_READY);assert(apply_worker_connection_closed(&ledger,0,7,out.connection_id)==0);}
        else if(stage==1){assert(result==WORKER_CLOSED_PENDING);assert(advance_worker_closed_receive(channel[0],0,7,&receiver,&out)==-1);}
        else assert(result==WORKER_CLOSED_EOF || (result==-1 && errno==ECONNRESET));
        assert(reclaim_reaped_worker_connections(&ledger,0,7)==0);check(&ledger,1,0);
        assert(reclaim_reaped_worker_connections(&ledger,0,7)==1);
        assert(apply_worker_connection_closed(&ledger,0,7,id)==1);
        assert(ledger.workers[1].outstanding==1);
        assert(activate_parent_connection_worker(&ledger,0,8)==0);
        assert(apply_worker_connection_closed(&ledger,0,7,id)==1);
        assert(reclaim_reaped_worker_connections(&ledger,1,9)==0);check(&ledger,0,0);
        assert(read(client[1],&marker,1)==0);close(client[1]);close(channel[0]);destroy_parent_connection_ledger(&ledger);
    }
}
int main(void) {
    alarm(30);setvbuf(stdout,NULL,_IOLBF,0);size_t before=fd_count();
    test_accounting();puts("[PASS] ledger limits, early CLOSED, stale/duplicate/owner, generation wrap, per-worker reclaim");
    test_selected_capacity_bounds();puts("[PASS] selected W4/C256/G1152 capacity and retained SENDING/CLOSED accounting");
    test_partial_and_full_queue();puts("[PASS] bounded CLOSED ring, partial I/O, EINTR, actual EAGAIN/resume, EPIPE");
    test_bad_frames_and_ancillary();puts("[PASS] malformed/coalesced frames, SCM_RIGHTS/truncation cleanup, partial EOF");
    test_fifo_fd_completion_flow();puts("[PASS] FIFO -> actual FD handoff -> reverse CLOSED -> capacity resume; early completion");
    test_reaped_worker_boundaries();puts("[PASS] real SIGKILL before/partial/complete CLOSED, waitpid and exactly-once reclaim");
    assert(fd_count()==before);alarm(0);
    printf("storage: entry=%zu worker=%zu ledger=%zu token=%zu queue=%zu receiver=%zu\n",
        sizeof(struct parent_connection_entry),sizeof(struct parent_connection_worker),sizeof(struct parent_connection_ledger),
        sizeof(struct worker_connection_closed),sizeof(struct worker_closed_queue),sizeof(struct worker_closed_receiver));
    return 0;
}
