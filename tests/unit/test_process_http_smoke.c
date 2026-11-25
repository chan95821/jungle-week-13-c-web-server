#define _GNU_SOURCE
/* Minimal integration check: real parent loop + four worker reactors + C backends.
 * Deliberately not the broad regression/fault/load suite. File/CGI bridge pending. */
#include "../../src/parent_http_loop.h"
#include "../../src/worker_http_internal.h"
#include "../../src/worker_failure_channel.h"
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t running;
static void on_signal(int number){record_process_signal(&running,number);}
static size_t fd_count(void){
    DIR *directory=opendir("/proc/self/fd");assert(directory);struct dirent *entry;size_t count=0;
    while((entry=readdir(directory)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))++count;
    assert(closedir(directory)==0);return count;
}
static struct sockaddr_in address(uint16_t port){return(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(port),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};}
static uint16_t open_listener(int *fd){
    *fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(*fd>=0);struct sockaddr_in local=address(0);
    assert(bind(*fd,(struct sockaddr *)&local,sizeof(local))==0 && listen(*fd,128)==0);
    socklen_t size=sizeof(local);assert(getsockname(*fd,(struct sockaddr *)&local,&size)==0);return ntohs(local.sin_port);
}
static int connect_client(uint16_t port){
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);struct sockaddr_in local=address(port);
    assert(connect(fd,(struct sockaddr *)&local,sizeof(local))==0);return fd;
}
static pid_t start_backend(const char *identity,const char *mode,uint16_t *port,int notify){
    int listener;*port=open_listener(&listener);close(listener);char port_text[16];snprintf(port_text,sizeof(port_text),"%u",*port);
    pid_t child=fork();assert(child>=0);
    if(!child){alarm(20);if(notify>=0)assert(dup2(notify,STDERR_FILENO)==STDERR_FILENO);
        execl("./build/test_backend","test_backend",port_text,identity,mode,(char *)NULL);_exit(127);}
    for(size_t attempt=0;attempt<200;++attempt){
        int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);struct sockaddr_in local=address(*port);
        int result=connect(fd,(struct sockaddr *)&local,sizeof(local));close(fd);if(!result)return child;
        struct timespec delay={0,5000000};nanosleep(&delay,NULL);
    }
    assert(!"C backend did not start");return -1;
}
static int run_worker(const struct proxy_config *config,struct shared_backend_memory *shared,
    uint32_t index,uint64_t generation,int channel,int failure,struct process_signal_wait *signals){
    struct worker_http_reactor reactor={0};
    if(create_worker_http_reactor(config,shared,index,generation,INITIAL_WORKER_CONNECTION_LIMIT,channel,&reactor)<0)return 1;
    int result;
    while((result=advance_worker_http_reactor(&reactor,signals,-1))==0){

    }
    if(atomic_load(&shared->header->fatal)){
        publish_worker_failure(shared,index,generation,&reactor.diagnostic);
        notify_parent_worker_failure(failure);
    }
    destroy_worker_http_reactor(&reactor);return result<0?1:0;
}
static int run_parent_server(const struct proxy_config *config,int listener,int report){
    alarm(20);size_t initial_fds=fd_count();
    struct process_signal_wait signals={0};assert(prepare_process_signal_wait(&signals,&running,on_signal)==0);
    struct shared_backend_memory shared={0};assert(create_shared_backend_memory(INITIAL_WORKER_COUNT,
        INITIAL_BACKEND_RESERVATION_CAPACITY,INITIAL_SHARED_MEMORY_BUDGET,&shared)==0);
    struct parent_connection_ledger ledger={0};assert(create_parent_connection_ledger(INITIAL_TOTAL_CONNECTION_LIMIT,
        INITIAL_WORKER_COUNT,INITIAL_WORKER_CONNECTION_LIMIT,&ledger)==0);
    struct parent_client_queue queue={0};assert(create_parent_client_queue(INITIAL_PARENT_QUEUE_CAPACITY,&queue)==0);
    struct parent_worker_supervisor supervisor={0};assert(create_parent_worker_supervisor(&ledger,&shared,&queue,&supervisor)==0);
    int failure[2];assert(create_worker_failure_pipe(failure)==0);
    struct parent_http_loop loop={0};assert(create_parent_http_loop(listener,failure[0],&supervisor,config,&signals,&loop)==0);
    uint64_t generations[INITIAL_WORKER_COUNT];assert(try_lock_shared_backend_state(&shared,NULL)==0);
    for(uint32_t i=0;i<INITIAL_WORKER_COUNT;++i)assert(activate_backend_worker_locked(&shared,i,&generations[i],NULL)==0);
    assert(unlock_shared_backend_state(&shared)==0);
    for(uint32_t i=0;i<INITIAL_WORKER_COUNT;++i){
        assert(activate_parent_connection_worker(&ledger,i,generations[i])==0 && arm_worker_failure_slot(&shared,i,generations[i])==0);
        int channel[2];assert(create_worker_fd_channel(channel)==0);pid_t child=fork();assert(child>=0);
        if(!child){
            close(report);close(listener);close(loop.epoll_fd);close(failure[0]);close(channel[0]);
            for(size_t prior=0;prior<i;++prior)close(supervisor.workers[prior].channel_fd);
            int result=run_worker(config,&shared,i,generations[i],channel[1],failure[1],&signals);
            close(channel[1]);close(failure[1]);_exit(result);
        }
        close(channel[1]);assert(adopt_parent_worker(&supervisor,i,generations[i],child,&channel[0])==0);
    }
    close(failure[1]);assert(finish_parent_worker_startup(&supervisor)==0);
    assert(write(report,"R",1)==1);
    int result=run_parent_http_loop(&loop);
    if(result)fprintf(stderr,"parent-smoke failed result=%d operation=%u errno=%d\n",result,
        (unsigned int)supervisor.diagnostic.failed_operation,supervisor.diagnostic.system_error);
    assert(parent_supervisor_all_reaped(&supervisor) && parent_supervisor_cleanup_finished(&supervisor));
    assert(ledger.total==0 && queue.count==0 && validate_parent_connection_ledger(&ledger)==0);
    assert(try_lock_shared_backend_state(&shared,NULL)==0 && validate_backend_reservations_locked(&shared,NULL)==0);
    for(size_t backend=0;backend<SHARED_BACKEND_COUNT;++backend)assert(shared.header->outstanding[backend]==0);
    assert(unlock_shared_backend_state(&shared)==0);
    destroy_parent_http_loop(&loop);assert(destroy_parent_worker_supervisor(&supervisor)==0);
    assert(destroy_parent_client_queue(&queue)==0);destroy_parent_connection_ledger(&ledger);
    assert(destroy_shared_backend_memory(&shared)==0);close(failure[0]);assert(restore_process_signal_wait(&signals)==0);
    assert(fd_count()==initial_fds);assert(write(report,"C",1)==1);close(report);close(listener);return result;
}
static size_t read_wire(int client,unsigned char *wire,size_t capacity){
    size_t used=0;
    while(used<capacity-1){struct pollfd watch={.fd=client,.events=POLLIN};assert(poll(&watch,1,3000)>0);
        ssize_t count=recv(client,wire+used,capacity-1-used,0);assert(count>=0);if(!count)break;used+=(size_t)count;
    }
    wire[used]=0;return used;
}
static size_t request(uint16_t port,const char *header,const void *body,size_t body_length,unsigned char *wire,size_t capacity){
    int client=connect_client(port);assert(send_all(client,header,strlen(header))==0);
    if(body_length)assert(send_all(client,body,body_length)==0);
    assert(shutdown(client,SHUT_WR)==0);size_t length=read_wire(client,wire,capacity);close(client);return length;
}
static void write_fixture(const char *path,const void *bytes,size_t length){
    int fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0);size_t offset=0;
    while(offset<length){ssize_t count=write(fd,(const unsigned char *)bytes+offset,length-offset);assert(count>0);offset+=(size_t)count;}
    assert(close(fd)==0);
}
int main(void){
    alarm(25);signal(SIGPIPE,SIG_IGN);setvbuf(stdout,NULL,_IOLBF,0);size_t initial_fds=fd_count();
    int marker[2];assert(pipe2(marker,O_CLOEXEC)==0);uint16_t ports[3];pid_t backends[3];
    backends[0]=start_backend("submission","normal",&ports[0],-1);
    backends[1]=start_backend("query-a","notify-silent",&ports[1],marker[1]);
    backends[2]=start_backend("query-b","normal",&ports[2],-1);close(marker[1]);
    char original[4096];assert(getcwd(original,sizeof(original)));
    char fixture[]="/tmp/jungle-file-smoke-XXXXXX";assert(mkdtemp(fixture) && chdir(fixture)==0);
    assert(mkdir("public",0700)==0 && mkdir("public/pages",0700)==0 && mkdir("public/assets",0700)==0);
    unsigned char *binary=malloc(131072);assert(binary);
    for(size_t i=0;i<131072;++i)binary[i]=(unsigned char)(i*37);
    write_fixture("public/assets/fixture.png",binary,131072);
    write_fixture("public/assets/empty.png","",0);
    write_fixture("public/pages/cafe.html","<h1>fixture</h1>",16);
    struct proxy_config config={.submission_backend=address(ports[0]),.query_backends={address(ports[1]),address(ports[2])},
        .worker_policy=DISTRIBUTE_RR,.query_policy=DISTRIBUTE_RR};
    int listener,report[2];uint16_t port=open_listener(&listener);assert(pipe2(report,O_CLOEXEC)==0);
    pid_t parent=fork();assert(parent>=0);
    if(!parent){close(report[0]);close(marker[0]);exit(run_parent_server(&config,listener,report[1]));}
    close(report[1]);close(listener);char state_byte;assert(read(report[0],&state_byte,1)==1 && state_byte=='R');
    int slow=connect_client(port);static const char get[]="GET /api/hello HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send_all(slow,get,sizeof(get)-1)==0);struct pollfd notification={.fd=marker[0],.events=POLLIN};assert(poll(&notification,1,2000)>0);
    char received[64];assert(read(marker[0],received,sizeof(received))>0);
    unsigned char wire[16384];size_t length=request(port,get,NULL,0,wire,sizeof(wire));assert(length && strstr((char *)wire,"200 OK") && strstr((char *)wire,"X-Backend-ID: query-b"));
    static const char hello[]="GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    length=request(port,hello,NULL,0,wire,sizeof(wire));assert(length>=5 && !memcmp(wire+length-5,"Hello",5));
    const unsigned char body[]={'a',0,'b'};static const char submit[]="POST /api/submissions HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\n";
    length=request(port,submit,body,sizeof(body),wire,sizeof(wire));assert(strstr((char *)wire,"201 Created") && length>=3 && !memcmp(wire+length-3,body,3));
    static const char greet[]="GET /greet?name=Alice HTTP/1.1\r\nHost: x\r\n\r\n";
    length=request(port,greet,NULL,0,wire,sizeof(wire));assert(length && strstr((char *)wire,"200 OK") && strstr((char *)wire,"text/html") && strstr((char *)wire,"Alice"));
    unsigned char *file_wire=malloc(132096);assert(file_wire);
    static const char file_request[]="GET /assets/fixture.png HTTP/1.1\r\nHost: x\r\n\r\n";
    length=request(port,file_request,NULL,0,file_wire,132096);
    unsigned char *separator=(unsigned char *)strstr((char *)file_wire,"\r\n\r\n");assert(separator);
    size_t header_length=(size_t)(separator+4-file_wire);
    assert(strstr((char *)file_wire,"200 OK") && strstr((char *)file_wire,"Content-Type: image/png") &&
        strstr((char *)file_wire,"Content-Length: 131072") && length-header_length==131072 && !memcmp(separator+4,binary,131072));
    static const char empty_request[]="GET /assets/empty.png HTTP/1.1\r\nHost: x\r\n\r\n";
    length=request(port,empty_request,NULL,0,wire,sizeof(wire));assert(length && strstr((char *)wire,"200 OK") && strstr((char *)wire,"Content-Length: 0"));
    static const char missing[]="GET /assets/missing.png HTTP/1.1\r\nHost: x\r\n\r\n";
    length=request(port,missing,NULL,0,wire,sizeof(wire));assert(length>=9 && strstr((char *)wire,"404 Not Found") && !memcmp(wire+length-9,"Not Found",9));
    static const char cafe[]="GET /cafe HTTP/1.1\r\nHost: x\r\n\r\n";
    length=request(port,cafe,NULL,0,wire,sizeof(wire));assert(length>=16 && strstr((char *)wire,"200 OK") && !memcmp(wire+length-16,"<h1>fixture</h1>",16));
    free(file_wire);free(binary);
    struct pollfd still_waiting={.fd=slow,.events=POLLIN};assert(poll(&still_waiting,1,0)==0);
    puts("[PASS] one listener -> four workers -> actual reactor/proxy; fast query/local/submission/static binary/empty/error complete while slow query remains pending");
    assert(kill(parent,SIGTERM)==0);int status;assert(waitpid(parent,&status,0)==parent && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(read(report[0],&state_byte,1)==1 && state_byte=='C');close(report[0]);close(marker[0]);close(slow);
    for(size_t i=0;i<3;++i){assert(kill(backends[i],SIGTERM)==0);assert(waitpid(backends[i],&status,0)==backends[i]);}
    assert(unlink("public/assets/fixture.png")==0 && unlink("public/assets/empty.png")==0 && unlink("public/pages/cafe.html")==0);
    assert(rmdir("public/assets")==0 && rmdir("public/pages")==0 && rmdir("public")==0 && chdir(original)==0 && rmdir(fixture)==0);
    assert(fd_count()==initial_fds);alarm(0);
    puts("[PASS] parent SIGTERM reaps four workers, closes pending clients, returns FD/connection/backend reservations to baseline");
    return 0;
}
