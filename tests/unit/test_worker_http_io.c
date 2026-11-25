#define _GNU_SOURCE
#include "../../src/worker_http_internal.h"
#include "../../src/parent_connection_ledger.h"
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t count_open_fds(void){
    DIR *directory=opendir("/proc/self/fd");assert(directory);struct dirent *entry;size_t count=0;
    while((entry=readdir(directory)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))++count;
    assert(closedir(directory)==0);return count;
}
static volatile sig_atomic_t worker_state;
static void worker_signal(int number){record_process_signal(&worker_state,number);}
static uint16_t bind_loopback(int *fd){
    *fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(*fd>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    assert(bind(*fd,(struct sockaddr *)&address,sizeof(address))==0 && listen(*fd,16)==0);
    socklen_t length=sizeof(address);assert(getsockname(*fd,(struct sockaddr *)&address,&length)==0);return ntohs(address.sin_port);
}
static struct sockaddr_in endpoint(uint16_t port){return(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(port),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};}
static uint64_t clock_ms(void){struct timespec time;uint64_t now;assert(worker_http_now(&time,&now)==0);return now;}
static void send_bytes(int fd,const void *data,size_t length){assert(send_all(fd,data,length)==0);}
static unsigned char *read_response(int fd,size_t *length){
    size_t capacity=8192;unsigned char *wire=malloc(capacity);assert(wire);*length=0;
    for(;;){struct pollfd watch={.fd=fd,.events=POLLIN};assert(poll(&watch,1,8500)>0);
        if(*length==capacity){capacity*=2;assert(capacity<=8*1024*1024);wire=realloc(wire,capacity);assert(wire);}
        ssize_t count=recv(fd,wire+*length,capacity-*length,0);assert(count>=0);if(!count)break;*length+=(size_t)count;
    }
    if(*length==capacity){wire=realloc(wire,capacity+1);assert(wire);}wire[*length]=0;return wire;
}
static size_t count_status(const unsigned char *wire,size_t length){size_t count=0;for(size_t i=0;i+9<=length;++i)if(!memcmp(wire+i,"HTTP/1.1 ",9))++count;return count;}
static size_t find_header_end(const unsigned char *wire,size_t length){for(size_t i=0;i+4<=length;++i)if(!memcmp(wire+i,"\r\n\r\n",4))return i+4;return 0;}
struct fixture {
    struct shared_backend_memory shared;
    struct parent_connection_ledger ledger;
    struct proxy_config config;
    pid_t backends[3],worker;
    int channel,listener,notify;
    uint16_t client_port;
    struct worker_closed_receiver completed;
};
static pid_t start_backend(const char *identity,const char *mode,uint16_t *port){
    int temporary;*port=bind_loopback(&temporary);close(temporary);char text[16];snprintf(text,sizeof(text),"%u",*port);
    pid_t child=fork();assert(child>=0);if(!child){alarm(35);execl("./build/test_backend","test_backend",text,identity,mode,(char *)NULL);_exit(127);}
    uint64_t due=clock_ms()+2000;
    for(;;){int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);struct sockaddr_in address=endpoint(*port);
        int result=connect(fd,(struct sockaddr *)&address,sizeof(address));close(fd);if(!result)break;
        assert(clock_ms()<due);struct timespec pause={0,5000000};nanosleep(&pause,NULL);
    }
    return child;
}
static void create_policy_fixture(struct fixture *f,const char *query_mode,enum distribution_policy policy){
    *f=(struct fixture){.channel=-1,.listener=-1,.notify=-1};
    const char *identities[]={"submission","query-a","query-b"};uint16_t ports[3];
    for(size_t i=0;i<3;++i)f->backends[i]=start_backend(identities[i],i==1?query_mode:"normal",&ports[i]);
    f->config=(struct proxy_config){.submission_backend=endpoint(ports[0]),.query_backends={endpoint(ports[1]),endpoint(ports[2])},.worker_policy=DISTRIBUTE_RR,.query_policy=policy};
    assert(create_shared_backend_memory(1,8,1024*1024,&f->shared)==0);
    assert(create_parent_connection_ledger(8,1,8,&f->ledger)==0);
    assert(try_lock_shared_backend_state(&f->shared,NULL)==0);uint64_t generation;assert(activate_backend_worker_locked(&f->shared,0,&generation,NULL)==0 && generation==1);
    assert(unlock_shared_backend_state(&f->shared)==0 && activate_parent_connection_worker(&f->ledger,0,1)==0);
    f->client_port=bind_loopback(&f->listener);int channel[2],ready[2],notify[2];
    assert(create_worker_fd_channel(channel)==0 && pipe(ready)==0 && pipe2(notify,O_NONBLOCK|O_CLOEXEC)==0);
    f->worker=fork();assert(f->worker>=0);
    if(!f->worker){
        alarm(35);close(channel[0]);close(ready[0]);close(notify[0]);close(f->listener);
        struct process_signal_wait signals={0};assert(prepare_process_signal_wait(&signals,&worker_state,worker_signal)==0);
        size_t initial_fds=count_open_fds();
        struct worker_http_reactor reactor={0};assert(create_worker_http_reactor(&f->config,&f->shared,0,1,8,channel[1],&reactor)==0);
        assert(write(ready[1],"R",1)==1);close(ready[1]);bool saw_pause=false,saw_resume=false,saw_backend=false;
        int result;
        while((result=advance_worker_http_reactor(&reactor,&signals,100))==0){
            bool paused=false;
            for(size_t slot=0;slot<reactor.capacity;++slot){
                struct worker_http_connection *c=&reactor.connections[slot];assert(worker_http_relay_used(c)<=WORKER_RELAY_BYTES);
                if(c->input_paused && !c->backend_finished)paused=true;
                if(!saw_backend && !strcmp(query_mode,"silent") && c->phase==WORKER_BACKEND_HEADER){saw_backend=true;assert(write(notify[1],"B",1)==1);}
                const struct http_request *request=worker_http_local_request(&reactor,slot);
                if(!request)continue;
                if(http_slice_equal(request->path,"/echo"))assert(resume_worker_http_memory_response(&reactor,slot,200,"application/octet-stream",request->body.data,request->body.length,NULL)==0);
                else if(http_slice_equal(request->path,"/") || http_slice_equal(request->path,"/hello"))assert(resume_worker_http_memory_response(&reactor,slot,200,"text/plain","Hello",5,NULL)==0);
                else _exit(80); /* Test never routes file/CGI; product main remains unchanged. */
            }
            if(paused && !saw_pause){saw_pause=true;assert(write(notify[1],"P",1)==1);}
            if(!paused && saw_pause && !saw_resume){saw_resume=true;assert(write(notify[1],"R",1)==1);}
        }
        if(result<0)fprintf(stderr,"worker-test-error errno=%d active=%zu\n",errno,reactor.active);
        destroy_worker_http_reactor(&reactor);assert(count_open_fds()==initial_fds-1);close(channel[1]);close(notify[1]);
        exit(result<0?81:0); /* Run sanitizer exit checks for the worker process too. */
    }
    close(channel[1]);close(ready[1]);close(notify[1]);f->channel=channel[0];f->notify=notify[0];
    char marker;assert(read(ready[0],&marker,1)==1);close(ready[0]);
}
static void create_fixture(struct fixture *f,const char *query_mode){create_policy_fixture(f,query_mode,DISTRIBUTE_RR);}
static int new_client(struct fixture *f,bool small_buffers,int accepted_seconds_ago){
    int client=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(client>=0);struct sockaddr_in address=endpoint(f->client_port);
    if(small_buffers){int size=4096;assert(setsockopt(client,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size))==0);}
    assert(connect(client,(struct sockaddr *)&address,sizeof(address))==0);int accepted=accept(f->listener,NULL,NULL);assert(accepted>=0);
    if(small_buffers){int size=4096;assert(setsockopt(accepted,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0);}
    struct timespec at;assert(clock_gettime(CLOCK_MONOTONIC,&at)==0);at.tv_sec-=accepted_seconds_ago;
    uint64_t id;assert(register_parent_connection(&f->ledger,&id)==0 && assign_parent_connection(&f->ledger,id,0,1)==0);
    struct worker_client_metadata metadata={0,1,id,at};struct worker_fd_sender sender={0};assert(prepare_worker_fd_transfer(&sender,&accepted,&metadata)==0);
    for(size_t attempt=0;attempt<100;++attempt){int result=advance_worker_fd_send(f->channel,&sender);assert(result>=0);if(result==WORKER_FD_READY)break;struct pollfd watch={f->channel,POLLOUT,0};assert(poll(&watch,1,1000)>0);}
    assert(sender.finished && finish_parent_connection_handoff(&f->ledger,id)==0);return client;
}
static void collect_closed(struct fixture *f){
    uint64_t due=clock_ms()+4000;
    while(f->ledger.total){
        struct worker_connection_closed closed={0};int result=advance_worker_closed_receive(f->channel,0,1,&f->completed,&closed);assert(result>=0 && result!=WORKER_CLOSED_EOF);
        if(result==WORKER_CLOSED_READY){assert(apply_worker_connection_observed_closed(&f->ledger,0,1,closed.connection_id,&closed.observation)==0);continue;}
        assert(clock_ms()<due);struct pollfd watch={f->channel,POLLIN,0};assert(poll(&watch,1,1000)>0);
    }
    assert(validate_parent_connection_ledger(&f->ledger)==0);
}
static void destroy_fixture(struct fixture *f){
    collect_closed(f);assert(kill(f->worker,SIGTERM)==0);int status;assert(waitpid(f->worker,&status,0)==f->worker && WIFEXITED(status) && WEXITSTATUS(status)==0);
    close(f->channel);close(f->listener);close(f->notify);
    assert(try_lock_shared_backend_state(&f->shared,NULL)==0 && reclaim_backend_worker_locked(&f->shared,0,1,NULL)==0);
    assert(validate_backend_reservations_locked(&f->shared,NULL)==0 && unlock_shared_backend_state(&f->shared)==0);
    for(size_t i=0;i<3;++i){assert(kill(f->backends[i],SIGTERM)==0);assert(waitpid(f->backends[i],&status,0)==f->backends[i]);}
    destroy_parent_connection_ledger(&f->ledger);assert(destroy_shared_backend_memory(&f->shared)==0);
}
static unsigned char *get_path(struct fixture *f,const char *path,size_t *length){
    int client=new_client(f,false,0);char header[512];int n=snprintf(header,sizeof(header),"GET %s HTTP/1.1\r\nHost: original.example\r\n\r\n",path);assert(n>0);
    send_bytes(client,header,(size_t)n);shutdown(client,SHUT_WR);unsigned char *wire=read_response(client,length);close(client);return wire;
}
static void test_basic_and_post(void){
    struct fixture f;create_fixture(&f,"normal");size_t length;
    unsigned char *wire=get_path(&f,"/hello",&length);assert(strstr((char *)wire,"200 OK") && length>=5 && !memcmp(wire+length-5,"Hello",5));free(wire);
    wire=get_path(&f,"/api/hello",&length);assert(strstr((char *)wire,"X-Backend-ID: query-a") && strstr((char *)wire,"Via: 1.1 jungle-proxy"));free(wire);
    wire=get_path(&f,"/api/submission-status",&length);assert(strstr((char *)wire,"X-Backend-ID: query-b"));free(wire);
    int client=new_client(&f,false,0);static const char header[]="POST /api/submissions?x=1 HTTP/1.1\r\nHost: original.example\r\nContent-Length: 65536\r\nExpect: 100-continue\r\nContent-Type: application/octet-stream\r\n\r\n";
    send_bytes(client,header,9);send_bytes(client,header+9,sizeof(header)-1-9);
    unsigned char interim[25];size_t used=0;while(used<25){ssize_t n=recv(client,interim+used,25-used,0);assert(n>0);used+=(size_t)n;}
    assert(!memcmp(interim,"HTTP/1.1 100 Continue\r\n\r\n",25));
    unsigned char body[65536];for(size_t i=0;i<sizeof(body);++i)body[i]=(unsigned char)(i%251);
    send_bytes(client,body,3);send_bytes(client,body+3,sizeof(body)-3);shutdown(client,SHUT_WR);wire=read_response(client,&length);close(client);
    size_t end=find_header_end(wire,length);assert(end && strstr((char *)wire,"201 Created") && length-end==sizeof(body) && !memcmp(wire+end,body,sizeof(body)));
    assert(strstr((char *)wire,"X-Received-Target: /api/submissions?x=1") && strstr((char *)wire,"X-Received-Expect: absent"));free(wire);
    destroy_fixture(&f);
}
static void test_slow_connections_do_not_block(void){
    struct fixture f;create_fixture(&f,"silent");
    int partial=new_client(&f,false,0);send_bytes(partial,"GET /hello HTTP/1.1\r\n",21);
    int slow=new_client(&f,false,0);static const char request[]="GET /api/hello HTTP/1.1\r\nHost: x\r\n\r\n";send_bytes(slow,request,sizeof(request)-1);shutdown(slow,SHUT_WR);
    size_t length;unsigned char *wire=get_path(&f,"/echo",&length);assert(strstr((char *)wire,"405 Method Not Allowed"));free(wire);
    wire=get_path(&f,"/hello",&length);assert(strstr((char *)wire,"200 OK"));free(wire);
    wire=get_path(&f,"/api/hello",&length);assert(strstr((char *)wire,"X-Backend-ID: query-b"));free(wire);
    assert(f.shared.header->outstanding[1]==1); /* Slow A still pending after B/local completed. */
    int submission=new_client(&f,false,0);static const char post[]="POST /api/submissions HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc";
    send_bytes(submission,post,sizeof(post)-1);shutdown(submission,SHUT_WR);wire=read_response(submission,&length);close(submission);assert(strstr((char *)wire,"201 Created"));free(wire);
    shutdown(partial,SHUT_WR);wire=read_response(partial,&length);close(partial);assert(strstr((char *)wire,"400 Bad Request"));free(wire);
    wire=read_response(slow,&length);close(slow);assert(strstr((char *)wire,"504 Gateway Timeout"));free(wire);destroy_fixture(&f);
}
static void test_framing_modes(void){
    const char *modes[]={"chunked","close","informational","no-content","not-modified","truncate","bad-chunk","bad-length","http10","http10-custom","body-timeout","http10-close"};
    for(size_t i=0;i<sizeof(modes)/sizeof(modes[0]);++i){struct fixture f;create_fixture(&f,modes[i]);size_t length;unsigned char *wire=get_path(&f,"/api/hello",&length);
        assert(length && find_header_end(wire,length));
        if(i==0){assert(strstr((char *)wire,"Transfer-Encoding: chunked"));assert(memmem(wire,length,"X-End: complete",15));}
        else if(i==1)assert(strstr((char *)wire,"Transfer-Encoding: chunked"));
        else if(i==2)assert(count_status(wire,length)==2 && strstr((char *)wire,"103 Early Hints"));
        else if(i==3)assert(strstr((char *)wire,"204 No Content") && !strstr((char *)wire,"Content-Type:"));
        else if(i==4)assert(strstr((char *)wire,"304 Not Modified") && strstr((char *)wire,"Content-Length: 123"));
        else if(i==5 || i==6)assert(count_status(wire,length)==1);
        else if(i==7)assert(strstr((char *)wire,"502 Bad Gateway"));
        else if(i==8)assert(strstr((char *)wire,"HTTP/1.1 200 OK") && length>=5 && !memcmp(wire+length-5,"hello",5));
        else if(i==9)assert(strstr((char *)wire,"HTTP/1.1 409 Legacy conflict"));
        else if(i==11)assert(strstr((char *)wire,"Transfer-Encoding: chunked"));
        else if(i==10)assert(count_status(wire,length)==1 && strstr((char *)wire,"200 OK") && !strstr((char *)wire,"504 Gateway Timeout"));
        else assert(strstr((char *)wire,"HTTP/1.1 "));
        free(wire);destroy_fixture(&f);
    }
}
static void test_error_boundaries_and_lock_wait(void){
    struct fixture f;create_fixture(&f,"normal");
    const char *bad[]={"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 65537\r\nExpect: 100-continue\r\n\r\n",
        "POST /api/submissions HTTP/1.1\r\nHost: x\r\nConnection: Host\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n"};
    for(size_t i=0;i<2;++i){int client=new_client(&f,false,0);send_bytes(client,bad[i],strlen(bad[i]));shutdown(client,SHUT_WR);size_t length;unsigned char *wire=read_response(client,&length);close(client);assert(strstr((char *)wire,i?"400 Bad Request":"413 Content Too Large") && !strstr((char *)wire,"100 Continue"));free(wire);}
    int expired=new_client(&f,false,8);static const char hello[]="GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    send_bytes(expired,hello,sizeof(hello)-1);shutdown(expired,SHUT_WR);size_t length;unsigned char *wire=read_response(expired,&length);close(expired);assert(strstr((char *)wire,"408 Request Timeout"));free(wire);
    assert(try_lock_shared_backend_state(&f.shared,NULL)==0);
    int selected=new_client(&f,false,0);static const char query[]="GET /api/hello HTTP/1.1\r\nHost: x\r\n\r\n";send_bytes(selected,query,sizeof(query)-1);shutdown(selected,SHUT_WR);
    wire=read_response(selected,&length);close(selected);assert(strstr((char *)wire,"503 Service Unavailable"));free(wire);
    assert(unlock_shared_backend_state(&f.shared)==0);
    wire=get_path(&f,"/hello",&length);assert(strstr((char *)wire,"200 OK"));free(wire);
    collect_closed(&f);
    for(size_t bytes=8191;bytes<=8193;++bytes){
        char *header=malloc(bytes);assert(header);
        static const char prefix[]="GET /hello HTTP/1.1\r\nHost: x\r\nX-Fill: ";
        memcpy(header,prefix,sizeof(prefix)-1);memset(header+sizeof(prefix)-1,'A',bytes-(sizeof(prefix)-1)-4);memcpy(header+bytes-4,"\r\n\r\n",4);
        int client=new_client(&f,false,0);send_bytes(client,header,bytes);free(header);shutdown(client,SHUT_WR);
        wire=read_response(client,&length);close(client);assert(strstr((char *)wire,bytes<=8192?"200 OK":"431 Request Header Fields Too Large"));free(wire);
    }
    destroy_fixture(&f);
}
static void test_backpressure_and_rst(void){
    struct fixture f;create_fixture(&f,"large-body");int slow=new_client(&f,true,0);
    static const char query[]="GET /api/hello HTTP/1.1\r\nHost: x\r\n\r\n";send_bytes(slow,query,sizeof(query)-1);shutdown(slow,SHUT_WR);
    struct pollfd watch={f.notify,POLLIN,0};assert(poll(&watch,1,3000)>0);char marker;assert(read(f.notify,&marker,1)==1 && marker=='P');
    size_t length;unsigned char *wire=get_path(&f,"/hello",&length);assert(strstr((char *)wire,"200 OK"));free(wire);
    wire=read_response(slow,&length);close(slow);size_t end=find_header_end(wire,length);assert(end && length-end==4194304);
    for(size_t i=0;i<4194304;++i)assert(wire[end+i]==(unsigned char)((i%16384)%251));
    free(wire);
    int reset=new_client(&f,false,0);send_bytes(reset,query,sizeof(query)-1);struct linger linger={1,0};assert(setsockopt(reset,SOL_SOCKET,SO_LINGER,&linger,sizeof(linger))==0);close(reset);
    wire=get_path(&f,"/hello",&length);assert(strstr((char *)wire,"200 OK"));free(wire);destroy_fixture(&f);
}
static void test_configured_query_policies(void){
    for(int policy=DISTRIBUTE_RR;policy<=DISTRIBUTE_COMBINED;++policy){
        struct fixture f;create_policy_fixture(&f,"normal",(enum distribution_policy)policy);
        for(size_t request=0;request<4;++request){size_t length;unsigned char *wire=get_path(&f,"/api/hello",&length);
            assert(strstr((char *)wire,"200 OK") && (strstr((char *)wire,"X-Backend-ID: query-a") || strstr((char *)wire,"X-Backend-ID: query-b")));
            if(request<2)assert(strstr((char *)wire,request?"X-Backend-ID: query-b":"X-Backend-ID: query-a"));
            free(wire);
        }
        assert(try_lock_shared_backend_state(&f.shared,NULL)==0);
        assert(validate_backend_reservations_locked(&f.shared,NULL)==0 && f.shared.header->timing[1].representative_us && f.shared.header->timing[2].representative_us);
        assert(f.shared.header->query_selections==(policy>=DISTRIBUTE_RESPONSE_TIME?4U:0U));
        assert(unlock_shared_backend_state(&f.shared)==0);destroy_fixture(&f);
    }
}
static void test_shutdown_during_backend_wait(void){
    struct fixture f;create_fixture(&f,"silent");int client=new_client(&f,false,0);
    static const char query[]="GET /api/hello HTTP/1.1\r\nHost: x\r\n\r\n";
    send_bytes(client,query,sizeof(query)-1);
    struct pollfd watch={f.notify,POLLIN,0};assert(poll(&watch,1,2000)>0);char marker;assert(read(f.notify,&marker,1)==1 && marker=='B');
    assert(kill(f.worker,SIGTERM)==0);int status;assert(waitpid(f.worker,&status,0)==f.worker && WIFEXITED(status) && WEXITSTATUS(status)==0);
    close(client);close(f.channel);close(f.listener);close(f.notify);
    assert(f.shared.header->outstanding[1]==1); /* Exit did not invent shared release. */
    assert(reclaim_reaped_worker_connections(&f.ledger,0,1)==0 && !f.ledger.total);
    assert(try_lock_shared_backend_state(&f.shared,NULL)==0 && reclaim_backend_worker_locked(&f.shared,0,1,NULL)==0);
    assert(validate_backend_reservations_locked(&f.shared,NULL)==0 && !f.shared.header->outstanding[1]);
    assert(unlock_shared_backend_state(&f.shared)==0);
    for(size_t i=0;i<3;++i){assert(kill(f.backends[i],SIGTERM)==0);assert(waitpid(f.backends[i],&status,0)==f.backends[i]);}
    destroy_parent_connection_ledger(&f.ledger);assert(destroy_shared_backend_memory(&f.shared)==0);
}
int main(int argc,char **argv){
    alarm(90);signal(SIGPIPE,SIG_IGN);setvbuf(stdout,NULL,_IOLBF,0);size_t initial_fds=count_open_fds();
    if(argc==2 && !strcmp(argv[1],"--backpressure-only")){test_backpressure_and_rst();alarm(0);return 0;}
    test_basic_and_post();puts("[PASS] actual fork worker/FD channel/LT TCP: local handoff, RR, split binary 64KiB POST and 100 Continue");
    test_slow_connections_do_not_block();puts("[PASS] slow header/backend do not block other local/submission connections; header 504");
    test_framing_modes();puts("[PASS] chunked/trailer, EOF, 1xx, 204/304, malformed/truncated backend framing");
    test_error_boundaries_and_lock_wait();puts("[PASS] early 400/413, accept-time header deadline, shared selection-lock 503 and recovery");
    test_backpressure_and_rst();puts("[PASS] actual relay pause with slow reader, other connection progress, resume/binary integrity and RST recovery");
    test_configured_query_policies();puts("[PASS] all four configured query policies on real TCP, shared timing and selection counters");
    test_shutdown_during_backend_wait();puts("[PASS] SIGTERM during live backend wait, worker FD cleanup, parent waitpid then reservation reclaim");
    printf("worker storage: connection=%zu reactor=%zu; relay cap=%d\n",sizeof(struct worker_http_connection),sizeof(struct worker_http_reactor),WORKER_RELAY_BYTES);
    assert(count_open_fds()==initial_fds);alarm(0);return 0;
}
