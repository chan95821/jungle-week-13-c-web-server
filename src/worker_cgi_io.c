#define _GNU_SOURCE
#include "worker_http_internal.h"
#include "handlers/cgi_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
struct worker_cgi_state {
    pid_t pid;
    int fd,status;
    bool registered,reaped,eof,cancelled,killed,final_chunk;
    uint64_t read_due,stop_due,reap_retry;
    size_t header_used;
    char *content_type;
    unsigned char header[CGI_HEADER_MAX_BYTES],buffer[CGI_CHUNK_BUFFER_SIZE];
};
static void close_cgi_pipe(struct worker_http_reactor *reactor,struct worker_cgi_state *cgi){
    if(cgi->fd>=0){if(cgi->registered)epoll_ctl(reactor->epoll_fd,EPOLL_CTL_DEL,cgi->fd,NULL);close(cgi->fd);cgi->fd=-1;cgi->registered=false;}
}
void worker_cgi_cancel(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_cgi_state *cgi=reactor->connections[slot].cgi;if(!cgi || cgi->cancelled)return;
    cgi->cancelled=true;cgi->read_due=0;close_cgi_pipe(reactor,cgi);
    if(!cgi->reaped){if(kill(cgi->pid,SIGTERM)<0 && errno!=ESRCH)reactor->stopping=true;cgi->stop_due=now+CGI_STOP_GRACE_MS;}
}
int worker_cgi_update_interest(struct worker_http_reactor *reactor,size_t slot){
    struct worker_http_connection *c=&reactor->connections[slot];struct worker_cgi_state *cgi=c->cgi;
    if(!cgi || cgi->fd<0)return 0;
    bool read=!cgi->cancelled && !cgi->eof && (c->phase==WORKER_CGI_HEADER ||
        (c->phase==WORKER_CGI_BODY && c->memory_sent==c->memory_length));
    if(read==cgi->registered)return 0;
    struct epoll_event event={.events=EPOLLIN,.data.u64=((uint64_t)c->slot_generation<<32)|((uint64_t)slot<<2)|2};
    if(epoll_ctl(reactor->epoll_fd,read?EPOLL_CTL_ADD:EPOLL_CTL_DEL,cgi->fd,read?&event:NULL)<0)return -1;
    cgi->registered=read;return 0;
}
int worker_cgi_start(struct worker_http_reactor *reactor,size_t slot,const struct cgi_launch *launch,uint64_t now){
    if(!launch || !reactor->signals){errno=EINVAL;return -1;}
    struct worker_http_connection *c=&reactor->connections[slot];
    struct worker_cgi_state *cgi=calloc(1,sizeof(*cgi));if(!cgi)return -1;cgi->fd=-1;
    int output[2];if(pipe2(output,O_CLOEXEC)<0){free(cgi);return -1;}
    int flags=fcntl(output[0],F_GETFL);if(flags<0 || fcntl(output[0],F_SETFL,flags|O_NONBLOCK)<0){close(output[0]);close(output[1]);free(cgi);return -1;}
    char *arguments[]={launch->resolved,NULL};pid_t child=fork();
    if(child<0){close(output[0]);close(output[1]);free(cgi);return -1;}
    if(!child){
        /* Other threads' locks are inherited: ONLY async-signal-safe calls.
         * All strings and /dev/null were prepared before fork. */
        if(restore_exec_child_signals(reactor->signals)<0)_exit(127);
        close(output[0]);
        if(dup2(launch->input_fd,STDIN_FILENO)<0 || dup2(output[1],STDOUT_FILENO)<0)_exit(127);
        if(fcntl(STDIN_FILENO,F_SETFD,0)<0 || fcntl(STDOUT_FILENO,F_SETFD,0)<0)_exit(127);
        if(launch->input_fd>STDOUT_FILENO)close(launch->input_fd);
        if(output[1]>STDOUT_FILENO)close(output[1]);
        execve(launch->resolved,arguments,launch->env);_exit(127);
    }
    close(output[1]);cgi->pid=child;cgi->fd=output[0];cgi->read_due=now+CGI_READ_TIMEOUT_MS;
    c->cgi=cgi;c->phase=WORKER_CGI_HEADER;return worker_cgi_update_interest(reactor,slot);
}
static void fail_cgi(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    worker_http_error_response(reactor,slot,502,now);
}
static void queue_cgi_chunk(struct worker_http_connection *c,const unsigned char *bytes,size_t length){
    struct worker_cgi_state *cgi=c->cgi;char prefix[32];int size=snprintf(prefix,sizeof(prefix),"%zx\r\n",length);
    memmove(cgi->buffer+(size_t)size,bytes,length);memcpy(cgi->buffer,prefix,(size_t)size);
    memcpy(cgi->buffer+(size_t)size+length,"\r\n",2);
    c->memory_body=cgi->buffer;c->memory_length=(size_t)size+length+2;c->memory_sent=0;cgi->read_due=0;
}
void worker_cgi_advance(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];struct worker_cgi_state *cgi=c->cgi;
    if(!cgi || cgi->fd<0 || cgi->cancelled)return;
    bool header=c->phase==WORKER_CGI_HEADER;
    if(!header && (c->phase!=WORKER_CGI_BODY || c->memory_sent<c->memory_length))return;
    unsigned char *destination=header?cgi->header+cgi->header_used:cgi->buffer+32;
    size_t capacity=header?sizeof(cgi->header)-cgi->header_used:sizeof(cgi->buffer)-34;
    if(!capacity){fail_cgi(reactor,slot,now);return;}
    ssize_t count=read(cgi->fd,destination,capacity);
    if(count<0){if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)fail_cgi(reactor,slot,now);return;}
    if(!count){
        cgi->eof=true;cgi->read_due=0;close_cgi_pipe(reactor,cgi);
        if(header)fail_cgi(reactor,slot,now);
        return;
    }
    if(!header){queue_cgi_chunk(c,destination,(size_t)count);return;}
    cgi->header_used+=(size_t)count;size_t separator=0;
    size_t end=cgi_find_header_end(cgi->header,cgi->header_used,&separator);
    if(end==SIZE_MAX){if(cgi->header_used==sizeof(cgi->header))fail_cgi(reactor,slot,now);return;}
    int status;
    if(!cgi_parse_headers(cgi->header,end,&cgi->content_type,&status)){fail_cgi(reactor,slot,now);return;}
    int length=snprintf((char *)c->local_header,sizeof(c->local_header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",status,http_reason(status),cgi->content_type);
    if(length<0 || (size_t)length>=sizeof(c->local_header)){fail_cgi(reactor,slot,now);return;}
    c->local_header_length=(size_t)length;c->local_header_sent=0;c->phase=WORKER_CGI_BODY;
    c->client_write_waiting=true;c->output_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;
    size_t initial=cgi->header_used-end-separator;cgi->read_due=now+CGI_READ_TIMEOUT_MS;
    if(initial)queue_cgi_chunk(c,cgi->header+end+separator,initial);
}
void worker_cgi_output_drained(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];struct worker_cgi_state *cgi=c->cgi;if(!cgi)return;
    c->memory_body=NULL;c->memory_length=c->memory_sent=0;
    if(cgi->final_chunk){shutdown(c->client_fd,SHUT_WR);c->phase=WORKER_CLIENT_DRAIN;c->drain_due=now+DRAIN_DEADLINE_SECONDS*1000;}
    else if(!cgi->eof)cgi->read_due=now+CGI_READ_TIMEOUT_MS;
}
uint64_t worker_cgi_deadline(const struct worker_http_connection *c){
    const struct worker_cgi_state *cgi=c->cgi;if(!cgi)return 0;
    uint64_t due=cgi->read_due;
    if(cgi->stop_due && (!due || cgi->stop_due<due))due=cgi->stop_due;
    if(cgi->reap_retry && (!due || cgi->reap_retry<due))due=cgi->reap_retry;
    return due;
}
static void free_cgi(struct worker_http_connection *c){free(c->cgi->content_type);free(c->cgi);c->cgi=NULL;}
void worker_cgi_service(struct worker_http_reactor *reactor,uint64_t now){
    if(reactor->signals)take_process_child_signal(reactor->signals);
    for(size_t slot=0;slot<reactor->capacity;++slot){
        struct worker_http_connection *c=&reactor->connections[slot];struct worker_cgi_state *cgi=c->cgi;if(!cgi)continue;
        if(!cgi->reaped && (!cgi->reap_retry || now>=cgi->reap_retry)){
            pid_t result=waitpid(cgi->pid,&cgi->status,WNOHANG);cgi->reap_retry=0;
            if(result==cgi->pid){cgi->reaped=true;cgi->stop_due=0;}
            else if(result<0 && errno==EINTR)cgi->reap_retry=now+INITIAL_WORKER_LOCK_RETRY_MS;
            else if(result<0){/* Ownership lost: never signal a potentially reused PID. */
                cgi->reaped=true;cgi->stop_due=0;reactor->stopping=true;}
        }
        if(cgi->cancelled){
            if(cgi->reaped){free_cgi(c);continue;}
            if(cgi->stop_due && now>=cgi->stop_due && !cgi->killed){
                if(kill(cgi->pid,SIGKILL)<0 && errno!=ESRCH)reactor->stopping=true;
                cgi->killed=true;cgi->stop_due=0;
            }
            continue;
        }
        if(cgi->read_due && now>=cgi->read_due){fail_cgi(reactor,slot,now);continue;}
        if(cgi->eof && cgi->reaped && !cgi->final_chunk && c->memory_sent==c->memory_length){
            if(!WIFEXITED(cgi->status) || WEXITSTATUS(cgi->status)){fail_cgi(reactor,slot,now);continue;}
            memcpy(cgi->buffer,"0\r\n\r\n",5);cgi->final_chunk=true;
            c->memory_body=cgi->buffer;c->memory_length=5;c->memory_sent=0;
        }
    }
}
void destroy_worker_cgi_children(struct worker_http_reactor *reactor){
    if(!reactor->connections)return;
    uint64_t now=0;struct timespec time;worker_http_now(&time,&now);
    for(size_t slot=0;slot<reactor->capacity;++slot)worker_cgi_cancel(reactor,slot,now);
    /* Exit boundary only; no service-loop waitpid/sleep. Reap CGI before joining
     * the disk helper so a blocked disk job cannot delay known-child cleanup. */
    for(unsigned int attempt=0;attempt<CGI_STOP_GRACE_MS/10;++attempt){
        bool pending=false;
        for(size_t slot=0;slot<reactor->capacity;++slot){
            struct worker_cgi_state *cgi=reactor->connections[slot].cgi;if(!cgi || cgi->reaped)continue;
            pid_t result=waitpid(cgi->pid,&cgi->status,WNOHANG);
            if(result==cgi->pid || (result<0 && errno==ECHILD))cgi->reaped=true;else pending=true;
        }
        if(!pending)break;
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    for(size_t slot=0;slot<reactor->capacity;++slot){
        struct worker_http_connection *c=&reactor->connections[slot];struct worker_cgi_state *cgi=c->cgi;if(!cgi)continue;
        if(!cgi->reaped){kill(cgi->pid,SIGKILL);pid_t result;
            do{result=waitpid(cgi->pid,&cgi->status,0);}while(result<0 && errno==EINTR);}
        close_cgi_pipe(reactor,cgi);free_cgi(c);
    }
}
