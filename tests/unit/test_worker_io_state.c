#define _GNU_SOURCE
#include "../../src/worker_http_internal.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t state;
static void on_signal(int number){record_process_signal(&state,number);}
struct state_fixture {struct worker_http_reactor reactor;struct shared_backend_memory memory;
    struct proxy_config config;struct process_signal_wait signals;int channel[2];};
static uint64_t now_ms(void){struct timespec time;uint64_t now;assert(worker_http_now(&time,&now)==0);return now;}
static void create(struct state_fixture *f){
    *f=(struct state_fixture){0};assert(create_worker_fd_channel(f->channel)==0);
    assert(create_shared_backend_memory(1,2,8192,&f->memory)==0);
    assert(try_lock_shared_backend_state(&f->memory,NULL)==0);uint64_t generation;
    assert(activate_backend_worker_locked(&f->memory,0,&generation,NULL)==0 && unlock_shared_backend_state(&f->memory)==0);
    assert(prepare_process_signal_wait(&f->signals,&state,on_signal)==0);
    assert(create_worker_http_reactor(&f->config,&f->memory,0,1,2,f->channel[1],&f->reactor)==0);
}
static void destroy(struct state_fixture *f){
    destroy_worker_http_reactor(&f->reactor);destroy_worker_http_reactor(&f->reactor);close(f->channel[0]);close(f->channel[1]);
    assert(try_lock_shared_backend_state(&f->memory,NULL)==0 && reclaim_backend_worker_locked(&f->memory,0,1,NULL)==0);
    assert(unlock_shared_backend_state(&f->memory)==0 && destroy_shared_backend_memory(&f->memory)==0);
    assert(restore_process_signal_wait(&f->signals)==0);
}
static struct worker_http_connection *connection(struct state_fixture *f,enum worker_http_phase phase){
    struct worker_http_connection *c=&f->reactor.connections[0];
    c->phase=phase;c->slot_generation=1;c->connection_id=(UINT64_C(1)<<32)|1;c->admitted_us=now_ms()*1000;
    f->reactor.active=1;return c;
}
static void test_io_budget(void){
    struct state_fixture f;create(&f);int backend[2];assert(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,backend)==0);
    int space=256*1024;assert(setsockopt(backend[0],SOL_SOCKET,SO_SNDBUF,&space,sizeof(space))==0);
    struct worker_http_connection *c=connection(&f,WORKER_BACKEND_REQUEST);c->backend_fd=backend[0];
    c->headers=calloc(1,sizeof(*c->headers));c->request_body=calloc(REQ_BODY_MAX_BYTES,1);assert(c->headers && c->request_body);
    c->backend_request_length=100;c->request.body=(struct http_slice){c->request_body,REQ_BODY_MAX_BYTES};
    struct worker_io_budget budget={0};worker_proxy_advance(&f.reactor,0,EPOLLOUT,now_ms(),&budget);
    assert(budget.bytes<=WORKER_EVENT_BYTES && budget.calls<=WORKER_EVENT_CALLS && c->request_sent>0);
    assert(c->request_sent<c->backend_request_length+c->request.body.length && c->phase==WORKER_BACKEND_REQUEST);
    close(backend[1]);destroy(&f);
    create(&f);assert(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,backend)==0);
    c=connection(&f,WORKER_BACKEND_HEADER);c->backend_fd=backend[0];c->headers=calloc(1,sizeof(*c->headers));c->response=calloc(1,sizeof(*c->response));assert(c->headers && c->response);
    assert(write(backend[1],"HTTP/1.1 200 OK\r\nX-Fill: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",55)==55);
    budget=(struct worker_io_budget){0};worker_proxy_advance(&f.reactor,0,EPOLLIN,now_ms(),&budget);
    assert(budget.calls==WORKER_EVENT_CALLS && c->headers->raw_header_length==WORKER_EVENT_CALLS);
    close(backend[1]);destroy(&f);
}
static void test_backpressure_deadlines(void){
    struct state_fixture f;create(&f);int backend[2],client[2];assert(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,backend)==0 && socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK,0,client)==0);
    struct worker_http_connection *c=connection(&f,WORKER_BACKEND_BODY);c->backend_fd=backend[0];c->client_fd=client[0];
    c->response=calloc(1,sizeof(*c->response));assert(c->response);c->response->framing=BACKEND_LENGTH;c->response->remaining=1;
    uint64_t now=now_ms();c->backend_due=now+15000;c->backend_idle_due=now+5000;c->relay_length=WORKER_RELAY_BYTES;
    struct worker_io_budget budget={0};worker_proxy_advance(&f.reactor,0,EPOLLIN,now,&budget);
    assert(c->input_paused && c->paused_idle_ms==5000 && !budget.calls);
    worker_proxy_timer(&f.reactor,0,now+6000);assert(c->phase==WORKER_BACKEND_BODY && c->backend_fd>=0);
    c->relay_offset=WORKER_RELAY_BYTES-WORKER_RELAY_LOW_BYTES+1;
    worker_http_resume_backend_input(c,now+6000);assert(!c->input_paused && c->backend_idle_due==now+11000 && c->backend_due==now+15000);
    c->relay_offset=0;c->relay_length=WORKER_RELAY_BYTES;budget=(struct worker_io_budget){0};worker_proxy_advance(&f.reactor,0,EPOLLIN,now+7000,&budget);
    assert(c->input_paused && c->paused_idle_ms==4000);
    worker_proxy_timer(&f.reactor,0,now+15000);
    assert(c->phase==WORKER_CLIENT_OUTPUT && c->backend_fd==-1 && strstr((char *)c->local_header,"504 Gateway Timeout"));
    close(backend[1]);close(client[1]);destroy(&f);
}
static void test_pending_release_retries(void){
    struct state_fixture f;create(&f);struct worker_http_connection *c=connection(&f,WORKER_CLIENT_OUTPUT);
    assert(try_lock_shared_backend_state(&f.memory,NULL)==0);
    assert(begin_backend_request_locked(&f.memory,&c->lifetime,0,1,0,1,0,NULL)==0 && unlock_shared_backend_state(&f.memory)==0);
    int ready[2],release[2];assert(pipe(ready)==0 && pipe(release)==0);pid_t child=fork();assert(child>=0);
    if(!child){alarm(5);close(ready[0]);close(release[1]);
        if(try_lock_shared_backend_state(&f.memory,NULL)<0)_exit(2);
        if(write(ready[1],"R",1)!=1)_exit(3);
        char byte;if(read(release[0],&byte,1)!=1)_exit(4);
        if(unlock_shared_backend_state(&f.memory)<0)_exit(5);
        _exit(0);
    }
    close(ready[1]);close(release[0]);char byte;assert(read(ready[0],&byte,1)==1);close(ready[0]);
    uint64_t now=now_ms();worker_http_backend_finished(&f.reactor,0,now,BACKEND_OUTCOME_COMPLETE);
    assert(c->lifetime.phase==BACKEND_REQUEST_RELEASE_PENDING && c->retry_at==now+10);
    worker_proxy_timer(&f.reactor,0,now+9);assert(c->retry_at==now+10);
    worker_proxy_timer(&f.reactor,0,now+10);assert(c->retry_at==now+20);
    worker_http_close_client(&f.reactor,0,now+11,BACKEND_OUTCOME_COMPLETE);assert(c->retry_at==now+20 && c->phase==WORKER_CLIENT_CLOSED);
    assert(f.memory.header->outstanding[1]==1);
    assert(write(release[1],"R",1)==1);close(release[1]);int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    worker_proxy_timer(&f.reactor,0,now+20);assert(c->lifetime.phase==BACKEND_REQUEST_RELEASED && c->retry_at==0 && !f.memory.header->outstanding[1]);
    destroy(&f);
}
static void test_stale_events_and_client_deadline(void){
    struct state_fixture f;create(&f);struct worker_http_connection *c=connection(&f,WORKER_LOCAL_READY);c->slot_generation=2;
    int pipefd[2];assert(pipe(pipefd)==0);struct epoll_event event={.events=EPOLLIN,.data.u64=UINT64_C(1)<<32};
    assert(epoll_ctl(f.reactor.epoll_fd,EPOLL_CTL_ADD,pipefd[0],&event)==0 && write(pipefd[1],"X",1)==1);
    assert(advance_worker_http_reactor(&f.reactor,&f.signals,0)==0 && c->phase==WORKER_LOCAL_READY && c->slot_generation==2);
    close(pipefd[0]);close(pipefd[1]);int client[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,client)==0);c->client_fd=client[0];c->phase=WORKER_CLIENT_OUTPUT;
    c->local_header_length=1;c->local_header[0]='X';c->client_write_waiting=true;uint64_t now=now_ms();c->output_idle_due=now+5000;
    worker_http_service_deadlines(&f.reactor,now+4999);assert(c->client_fd>=0);
    worker_http_service_deadlines(&f.reactor,now+5000);assert(f.reactor.active==0 && f.reactor.completed.count==1 && f.reactor.completed.items[0].observation.result==DISTRIBUTION_CANCELLED);
    close(client[1]);destroy(&f);
}
static void test_client_body_and_interim_deadlines(void){
    struct state_fixture f;create(&f);int peer[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,peer)==0);
    struct worker_http_connection *c=connection(&f,WORKER_CLIENT_BODY);c->client_fd=peer[0];uint64_t now=now_ms();
    c->body_due=now+15000;c->input_idle_due=now+16000;
    worker_http_service_deadlines(&f.reactor,now+14999);assert(c->phase==WORKER_CLIENT_BODY);
    worker_http_service_deadlines(&f.reactor,now+15000);assert(c->phase==WORKER_CLIENT_OUTPUT && strstr((char *)c->local_header,"408 Request Timeout"));
    close(peer[1]);destroy(&f);
    create(&f);assert(socketpair(AF_UNIX,SOCK_STREAM,0,peer)==0);
    c=connection(&f,WORKER_INTERIM_WRITE);c->client_fd=peer[0];now=now_ms();c->body_due=now+15000;
    memcpy(c->local_header,"HTTP/1.1 100 Continue\r\n\r\n",25);c->local_header_length=25;c->local_header_sent=1;
    c->output_idle_due=now+16000;c->client_write_waiting=true;
    worker_http_service_deadlines(&f.reactor,now+15000);
    assert(f.reactor.active==0 && f.reactor.completed.count==1);
    assert(f.reactor.completed.items[0].observation.result==DISTRIBUTION_CANCELLED);
    close(peer[1]);destroy(&f);
}
int main(void){
    alarm(20);setvbuf(stdout,NULL,_IOLBF,0);signal(SIGPIPE,SIG_IGN);
    test_io_budget();puts("[PASS] actual nonblocking request-send byte budget and response-header syscall budget");
    test_backpressure_deadlines();puts("[PASS] relay high/low pause, remaining idle preservation, absolute backend deadline survives pause");
    test_pending_release_retries();puts("[PASS] actual mutex contention: release token and 10ms retry retained across client cleanup");
    test_stale_events_and_client_deadline();puts("[PASS] real epoll stale-generation event ignored, write idle before/exact boundary and cancelled CLOSED");
    test_client_body_and_interim_deadlines();puts("[PASS] absolute client-body deadline; partial 100 response closes without splicing final error");
    alarm(0);return 0;
}
