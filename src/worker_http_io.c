#define _GNU_SOURCE
#include "worker_http_internal.h"
#include "handlers/handlers.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static void free_connection_storage(struct worker_http_connection *connection);

static uint64_t event_token(size_t slot,uint32_t generation,bool backend){return ((uint64_t)generation<<32)|((uint64_t)slot<<2)|backend;}
static int set_interest(int epoll_fd,int fd,uint32_t *current,uint32_t wanted,uint64_t token){
    if(fd<0)return 0;
    if(*current==wanted)return 0;
    struct epoll_event event={.events=wanted,.data.u64=token};
    int operation=!wanted?EPOLL_CTL_DEL:!*current?EPOLL_CTL_ADD:EPOLL_CTL_MOD;
    if(epoll_ctl(epoll_fd,operation,fd,wanted?&event:NULL)<0)return -1;
    *current=wanted;return 0;
}
int worker_http_now(struct timespec *time,uint64_t *milliseconds){
    if(clock_gettime(CLOCK_MONOTONIC,time)<0)return -1;
    if(time->tv_sec<0 || (uint64_t)time->tv_sec>(UINT64_MAX-60000)/1000){errno=EOVERFLOW;return -1;}
    *milliseconds=(uint64_t)time->tv_sec*1000+(uint64_t)time->tv_nsec/1000000;return 0;
}
size_t worker_http_relay_used(const struct worker_http_connection *c){return c->relay_length-c->relay_offset;}
int worker_http_update_interest(struct worker_http_reactor *reactor,size_t slot){
    struct worker_http_connection *c=&reactor->connections[slot];uint32_t client=0,backend=0;
    if(c->client_fd>=0){
        if(c->phase==WORKER_CLIENT_HEADER || c->phase==WORKER_CLIENT_BODY || c->phase==WORKER_CLIENT_DRAIN)client|=EPOLLIN;
        if(c->phase==WORKER_INTERIM_WRITE || c->local_header_sent<c->local_header_length ||
           c->memory_sent<c->memory_length || worker_http_relay_used(c) ||
           (c->headers && c->response_header_sent<c->headers->client_header_length && c->phase>=WORKER_BACKEND_BODY))client|=EPOLLOUT;
        if(!c->client_half_closed)client|=EPOLLRDHUP;
    }
    if(c->backend_fd>=0){
        if(c->phase==WORKER_BACKEND_CONNECT || c->phase==WORKER_BACKEND_REQUEST)backend=EPOLLOUT;
        else if(c->phase==WORKER_BACKEND_HEADER || (c->phase==WORKER_BACKEND_BODY && !c->input_paused))backend=EPOLLIN;
    }
    if(set_interest(reactor->epoll_fd,c->client_fd,&c->client_interest,client,event_token(slot,c->slot_generation,false))<0 ||
       set_interest(reactor->epoll_fd,c->backend_fd,&c->backend_interest,backend,event_token(slot,c->slot_generation,true))<0)return -1;
    return worker_cgi_update_interest(reactor,slot);
}
static void close_owned_socket(struct worker_http_reactor *reactor,int *fd,uint32_t *interest){
    if(*fd>=0){if(*interest)epoll_ctl(reactor->epoll_fd,EPOLL_CTL_DEL,*fd,NULL);int owned=*fd;*fd=-1;*interest=0;close(owned);}
}
void worker_http_backend_finished(struct worker_http_reactor *reactor,size_t slot,uint64_t now,enum backend_request_outcome outcome){
    struct worker_http_connection *c=&reactor->connections[slot];
    close_owned_socket(reactor,&c->backend_fd,&c->backend_interest);c->backend_finished=true;
    if(c->lifetime.phase==BACKEND_REQUEST_ACTIVE){
        struct timespec time;uint64_t ignored;
        if(c->lifetime.clock_started && worker_http_now(&time,&ignored)==0)
            finish_backend_request_clock(&c->lifetime,&time,outcome);
        else mark_backend_request_finished(&c->lifetime,outcome);
    }
    if(c->lifetime.phase==BACKEND_REQUEST_RELEASE_PENDING && (!c->retry_at || now>=c->retry_at)){
        int result=try_release_backend_request(reactor->shared,&c->lifetime,&reactor->diagnostic);
        if(result==1)c->retry_at=now+INITIAL_WORKER_LOCK_RETRY_MS;
        else if(result<0)reactor->stopping=true;
        else c->retry_at=0;
    }
}
void worker_http_close_client(struct worker_http_reactor *reactor,size_t slot,uint64_t now,enum backend_request_outcome outcome){
    struct worker_http_connection *c=&reactor->connections[slot];
    worker_cgi_cancel(reactor,slot,now);
    worker_http_backend_finished(reactor,slot,now,outcome);
    close_owned_socket(reactor,&c->client_fd,&c->client_interest);
    free_connection_storage(c);
    c->completion_result=outcome==BACKEND_OUTCOME_CANCELLED?DISTRIBUTION_CANCELLED:c->internal_failure?DISTRIBUTION_FAILED:DISTRIBUTION_COMPLETED;
    c->phase=WORKER_CLIENT_CLOSED;c->input_idle_due=c->output_idle_due=0;
}
static int prepare_memory_response(struct worker_http_connection *c,int status,const char *type,
    const void *body,size_t length,const char *allow,uint64_t now){
    char allow_line[48]="";
    if(allow){int n=snprintf(allow_line,sizeof(allow_line),"Allow: %s\r\n",allow);if(n<0 || (size_t)n>=sizeof(allow_line))return -1;}
    int n;
    if(c->head_request)n=snprintf((char *)c->local_header,sizeof(c->local_header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%sConnection: close\r\n\r\n",status,http_reason(status),type,allow_line);
    else n=snprintf((char *)c->local_header,sizeof(c->local_header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",status,http_reason(status),type,length,allow_line);
    if(n<0 || (size_t)n>=sizeof(c->local_header)){errno=EOVERFLOW;return -1;}
    c->local_header_length=(size_t)n;c->local_header_sent=0;c->memory_body=body;
    c->memory_length=c->head_request?0:length;c->memory_sent=0;c->phase=WORKER_CLIENT_OUTPUT;
    c->output_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;c->client_write_waiting=true;return 0;
}
int worker_http_error_response(struct worker_http_reactor *reactor,size_t slot,int status,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    worker_cgi_cancel(reactor,slot,now);
    if(status==500)c->internal_failure=true;
    if(c->final_started){worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_FAILED);return 0;}
    worker_http_backend_finished(reactor,slot,now,BACKEND_OUTCOME_FAILED);
    if(c->headers)c->headers->client_header_length=0;
    c->relay_length=c->relay_offset=0;
    const char *body=http_reason(status);
    return prepare_memory_response(c,status,"text/plain",body,strlen(body),c->allow,now);
}
static void prepare_known_memory_route(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];router_handler handler=c->route.local_handler;
    if(handler!=prepare_hello && handler!=prepare_echo && handler!=prepare_greet)return;
    /* These three existing handlers only parse/format caller-owned memory.
     * File/CGI providers must never be driven synchronously from this bridge. */
    handler(&c->request,&c->local_response);
    struct router_response *response=&c->local_response;struct http_slice body={0};
    if(response->length){
        if(!response->body_provider.next_func ||
           response->body_provider.next_func(response->body_provider.state,&body)!=BODY_DATA ||
           body.length!=response->length){worker_http_error_response(reactor,slot,500,now);return;}
    }
    if(resume_worker_http_memory_response(reactor,slot,response->status,response->content_type,
        body.data,body.length,response->allow)<0)worker_http_error_response(reactor,slot,500,now);
}
int worker_http_prepare_file_headers(struct worker_http_reactor *reactor,size_t slot,const struct router_response *response,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    if(prepare_memory_response(c,response->status,response->content_type,NULL,response->length,response->allow,now)<0)return -1;
    c->memory_length=0;c->memory_sent=0;c->phase=WORKER_FILE_OUTPUT;
    if(response->status==500)c->internal_failure=true;
    return 0;
}
static void request_ready(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    c->request.body=(struct http_slice){c->request_body,c->body_used};
    if(c->route.proxy_route==PROXY_ROUTE_LOCAL){
        c->phase=WORKER_LOCAL_READY;prepare_known_memory_route(reactor,slot,now);
        if(c->phase==WORKER_LOCAL_READY && worker_file_start_response(reactor,slot,now)<0)
            worker_http_error_response(reactor,slot,500,now);
        return;
    }
    c->phase=WORKER_BACKEND_SELECT;c->select_due=now+INITIAL_WORKER_SELECT_WAIT_MS;c->retry_at=now;
}
static void parse_client_headers(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    int status=http_parse_request(c->request_header,c->header_end,&c->request);
    c->head_request=http_slice_equal(c->request.method,"HEAD");
    if(!status)status=http_select_route(&c->request,&c->route,&c->allow);
    if(!status && c->route.proxy_route!=PROXY_ROUTE_LOCAL)status=proxy_validate_request(&c->request);
    if(status){worker_http_error_response(reactor,slot,status,now);return;}
    c->body_due=now+BODY_DEADLINE_SECONDS*1000;
    if(c->request.content_length){
        c->request_body=malloc(c->request.content_length);
        if(!c->request_body){worker_http_error_response(reactor,slot,500,now);return;}
        c->body_used=c->header_used-c->header_end;
        if(c->body_used>c->request.content_length)c->body_used=c->request.content_length;
        memcpy(c->request_body,c->request_header+c->header_end,c->body_used);
    }
    if(c->body_used==c->request.content_length){request_ready(reactor,slot,now);return;}
    c->input_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;
    if(c->request.expect_continue){
        static const char interim[]="HTTP/1.1 100 Continue\r\n\r\n";
        memcpy(c->local_header,interim,sizeof(interim)-1);c->local_header_length=sizeof(interim)-1;
        c->phase=WORKER_INTERIM_WRITE;c->output_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;
    }else c->phase=WORKER_CLIENT_BODY;
}
static void read_client_request(struct worker_http_reactor *reactor,size_t slot,uint64_t now,struct worker_io_budget *budget){
    struct worker_http_connection *c=&reactor->connections[slot];
    while(budget->calls<WORKER_EVENT_CALLS && budget->bytes<WORKER_EVENT_BYTES){
        unsigned char *destination;size_t remaining;
        bool header=c->phase==WORKER_CLIENT_HEADER;
        if(header){destination=c->request_header+c->header_used;remaining=sizeof(c->request_header)-c->header_used;}
        else {destination=c->request_body+c->body_used;remaining=c->request.content_length-c->body_used;}
        if(remaining>WORKER_EVENT_BYTES-budget->bytes)remaining=WORKER_EVENT_BYTES-budget->bytes;
        ++budget->calls;ssize_t count=recv(c->client_fd,destination,remaining,MSG_DONTWAIT);
        if(count<0){if(errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)return;worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);return;}
        if(!count){
            c->client_half_closed=true;
            if(header && !c->header_used)worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);
            else worker_http_error_response(reactor,slot,400,now);
            return;
        }
        budget->bytes+=(size_t)count;c->input_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;
        if(header){
            size_t previous=c->header_used;c->header_used+=(size_t)count;
            for(size_t i=previous>3?previous-3:0;i+4<=c->header_used;++i)
                if(!memcmp(c->request_header+i,"\r\n\r\n",4)){c->header_end=i+4;break;}
            if(c->header_end){parse_client_headers(reactor,slot,now);return;}
            if(c->header_used==sizeof(c->request_header)){worker_http_error_response(reactor,slot,431,now);return;}
        }else {c->body_used+=(size_t)count;if(c->body_used==c->request.content_length){request_ready(reactor,slot,now);return;}}
    }
}
static bool client_output_pending(const struct worker_http_connection *c){
    return c->local_header_sent<c->local_header_length || c->memory_sent<c->memory_length || worker_http_relay_used(c) ||
        (c->headers && c->phase>=WORKER_BACKEND_BODY && c->response_header_sent<c->headers->client_header_length);
}
void worker_http_resume_backend_input(struct worker_http_connection *c,uint64_t now){
    if(c->input_paused && worker_http_relay_used(c)<=WORKER_RELAY_LOW_BYTES){
        c->input_paused=false;c->backend_idle_due=now+c->paused_idle_ms;c->paused_idle_ms=0;
    }
}
static void write_client_output(struct worker_http_reactor *reactor,size_t slot,uint64_t now,struct worker_io_budget *budget){
    struct worker_http_connection *c=&reactor->connections[slot];
    while(budget->calls<WORKER_EVENT_CALLS && budget->bytes<WORKER_EVENT_BYTES && client_output_pending(c)){
        const unsigned char *data;size_t length;enum {LOCAL_HEADER,LOCAL_BODY,BACKEND_HEADER,RELAY} kind;
        if(c->local_header_sent<c->local_header_length){data=c->local_header+c->local_header_sent;length=c->local_header_length-c->local_header_sent;kind=LOCAL_HEADER;}
        else if(c->memory_sent<c->memory_length){data=c->memory_body+c->memory_sent;length=c->memory_length-c->memory_sent;kind=LOCAL_BODY;}
        else if(c->headers && c->response_header_sent<c->headers->client_header_length){data=c->headers->client_header+c->response_header_sent;length=c->headers->client_header_length-c->response_header_sent;kind=BACKEND_HEADER;}
        else {data=c->response->buffer+c->relay_offset;length=worker_http_relay_used(c);kind=RELAY;}
        if(length>WORKER_EVENT_BYTES-budget->bytes)length=WORKER_EVENT_BYTES-budget->bytes;
        if(c->phase!=WORKER_INTERIM_WRITE)c->final_started=true;
        ++budget->calls;ssize_t count=send(c->client_fd,data,length,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(count<0){if(errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)return;worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);return;}
        if(!count){worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);return;}
        budget->bytes+=(size_t)count;c->output_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;
        if(kind==LOCAL_HEADER)c->local_header_sent+=(size_t)count;
        else if(kind==LOCAL_BODY)c->memory_sent+=(size_t)count;
        else if(kind==BACKEND_HEADER)c->response_header_sent+=(size_t)count;
        else c->relay_offset+=(size_t)count;
        if(c->relay_offset==c->relay_length)c->relay_offset=c->relay_length=0;
        worker_http_resume_backend_input(c,now);
    }
    if(client_output_pending(c))return;
    c->client_write_waiting=false;c->output_idle_due=0;
    if(c->phase==WORKER_INTERIM_WRITE){
        c->local_header_length=c->local_header_sent=0;c->phase=WORKER_CLIENT_BODY;c->input_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;
    }else if(c->phase==WORKER_CGI_BODY){
        worker_cgi_output_drained(reactor,slot,now);
    }else if(c->phase==WORKER_FILE_OUTPUT){
        worker_file_output_drained(reactor,slot,now);
    }else if(c->phase==WORKER_CLIENT_OUTPUT || (c->phase==WORKER_BACKEND_BODY && c->backend_finished && !c->terminator_pending)){
        shutdown(c->client_fd,SHUT_WR);c->phase=WORKER_CLIENT_DRAIN;c->drain_due=now+DRAIN_DEADLINE_SECONDS*1000;
    }
}
static void drain_client(struct worker_http_reactor *reactor,size_t slot,uint64_t now,struct worker_io_budget *budget){
    struct worker_http_connection *c=&reactor->connections[slot];unsigned char discard[4096];
    while(budget->calls<WORKER_EVENT_CALLS && budget->bytes<WORKER_EVENT_BYTES && c->drain_bytes<DRAIN_MAX_BYTES){
        size_t size=DRAIN_MAX_BYTES-c->drain_bytes;if(size>sizeof(discard))size=sizeof(discard);
        if(size>WORKER_EVENT_BYTES-budget->bytes)size=WORKER_EVENT_BYTES-budget->bytes;
        ++budget->calls;ssize_t count=recv(c->client_fd,discard,size,MSG_DONTWAIT);
        if(count<0){if(errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)return;worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);return;}
        if(!count){worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_COMPLETE);return;}
        c->drain_bytes+=(size_t)count;budget->bytes+=(size_t)count;
    }
    if(c->drain_bytes==DRAIN_MAX_BYTES)worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_COMPLETE);
}
static void advance_client_event(struct worker_http_reactor *reactor,size_t slot,uint32_t events,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];struct worker_io_budget budget={0};
    if(events&EPOLLERR){int error=0;socklen_t size=sizeof(error);if(getsockopt(c->client_fd,SOL_SOCKET,SO_ERROR,&error,&size)<0 || error){worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);return;}}
    if(events&(EPOLLRDHUP|EPOLLHUP))c->client_half_closed=true;
    if(c->phase==WORKER_CLIENT_HEADER || c->phase==WORKER_CLIENT_BODY)read_client_request(reactor,slot,now,&budget);
    if(c->phase==WORKER_CLIENT_DRAIN)drain_client(reactor,slot,now,&budget);
    else if(c->client_fd>=0 && (events&EPOLLOUT))write_client_output(reactor,slot,now,&budget);
}
static int channel_interest(struct worker_http_reactor *reactor){
    uint32_t events=0;
    if(reactor->active+reactor->completed.count<reactor->capacity && worker_closed_has_capacity(&reactor->completed))events|=EPOLLIN;
    if(reactor->completed.count)events|=EPOLLOUT;
    struct epoll_event event={.events=events,.data.u64=WORKER_CHANNEL_EVENT};
    /* A channel remains registered even with no requested events: HUP/ERR still
     * terminate the borrowed parent transport. No level-triggered spin. */
    return epoll_ctl(reactor->epoll_fd,EPOLL_CTL_MOD,reactor->channel_fd,&event);
}
static int admit_received_client(struct worker_http_reactor *reactor,struct worker_received_client *received,uint64_t now){
    for(size_t slot=0;slot<reactor->capacity;++slot){
        struct worker_http_connection *c=&reactor->connections[slot];if(c->phase!=WORKER_SLOT_FREE)continue;
        if(c->slot_generation==UINT32_MAX){errno=EOVERFLOW;return -1;}
        uint32_t generation=c->slot_generation+1;
        if((uint64_t)received->metadata.accepted_at.tv_sec>(UINT64_MAX-60000)/1000){errno=EOVERFLOW;return -1;}
        uint64_t accepted=(uint64_t)received->metadata.accepted_at.tv_sec*1000+(uint64_t)received->metadata.accepted_at.tv_nsec/1000000;
        struct timespec admitted;uint64_t admitted_ms;struct distribution_observation point;
        if(worker_http_now(&admitted,&admitted_ms)<0 || distribution_elapsed_us(&admitted,&admitted,&point,DISTRIBUTION_COMPLETED)<0)return -1;
        *c=(struct worker_http_connection){.slot_generation=generation,.client_fd=received->client_fd,.backend_fd=-1,
            .phase=WORKER_CLIENT_HEADER,.connection_id=received->metadata.connection_id,.admitted_us=now*1000,
            .header_due=accepted+HEADER_DEADLINE_SECONDS*1000,.input_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000};
        c->admitted_us=point.finished_us;
        received->client_fd=-1;++reactor->active;return worker_http_update_interest(reactor,slot);
    }
    errno=ENOSPC;return -1;
}
static int advance_channel(struct worker_http_reactor *reactor,uint32_t events,uint64_t now){
    if(events&(EPOLLERR|EPOLLHUP)){errno=EPIPE;return -1;}
    unsigned int calls=0;
    if(events&EPOLLOUT)while(reactor->completed.count && calls++<WORKER_EVENT_CALLS){
        int result=advance_worker_closed_send(reactor->channel_fd,&reactor->completed);if(result<0)return -1;if(result==WORKER_CLOSED_PENDING)break;
    }
    if(events&EPOLLIN)while(reactor->active+reactor->completed.count<reactor->capacity && calls+4<=WORKER_EVENT_CALLS){
        /* recvmsg plus bounded SO_TYPE/getpeername/reset-evidence checks. */
        calls+=4;
        struct worker_received_client received={.client_fd=-1};
        int result=advance_worker_fd_receive(reactor->channel_fd,reactor->worker_index,reactor->worker_generation,reactor->incoming,&received);
        if(result<0)return -1;
        if(result==WORKER_FD_EOF){errno=EPIPE;return -1;}
        if(result==WORKER_FD_PENDING)break;
        if(result==WORKER_FD_CLIENT_GONE){
            struct worker_connection_closed cancelled={.worker_index=reactor->worker_index,
                .worker_generation=reactor->worker_generation,.connection_id=received.metadata.connection_id,
                .observation={0,now*1000,DISTRIBUTION_CANCELLED}};
            if(enqueue_worker_connection_closed(&reactor->completed,&cancelled)<0)return -1;
            continue;
        }
        if(admit_received_client(reactor,&received,now)<0){if(received.client_fd>=0)close(received.client_fd);return -1;}
    }
    return 0;
}
static void free_connection_storage(struct worker_http_connection *c){
    if(c->local_response.body_provider.cleanup_func)
        c->local_response.body_provider.cleanup_func(c->local_response.body_provider.state);
    c->local_response=(struct router_response){0};
    free(c->request_body);free(c->headers);
    if(c->response){free(c->response->connection_fields);free(c->response->trailer_buffer);free(c->response);}
    c->request_body=NULL;c->headers=NULL;c->response=NULL;
}
int worker_http_publish_completed(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    if(c->phase!=WORKER_CLIENT_CLOSED || c->file_job || c->cgi || c->lifetime.phase==BACKEND_REQUEST_RELEASE_PENDING)return 0;
    free_connection_storage(c);
    enum distribution_sample_result result=c->completion_result;
    struct timespec finished;uint64_t finished_ms;struct distribution_observation point;
    if(worker_http_now(&finished,&finished_ms)<0 || distribution_elapsed_us(&finished,&finished,&point,DISTRIBUTION_COMPLETED)<0)return -1;
    (void)now;
    struct worker_connection_closed completed={.worker_index=reactor->worker_index,
        .worker_generation=reactor->worker_generation,.connection_id=c->connection_id,
        .observation={point.finished_us-c->admitted_us,point.finished_us,result}};
    if(enqueue_worker_connection_closed(&reactor->completed,&completed)<0)return -1;
    uint32_t generation=c->slot_generation;
    *c=(struct worker_http_connection){.slot_generation=generation,.client_fd=-1,.backend_fd=-1};--reactor->active;return 0;
}
void worker_http_service_deadlines(struct worker_http_reactor *reactor,uint64_t now){
    worker_cgi_service(reactor,now);
    for(size_t slot=0;slot<reactor->capacity;++slot){
        struct worker_http_connection *c=&reactor->connections[slot];if(c->phase==WORKER_SLOT_FREE)continue;
        worker_proxy_timer(reactor,slot,now);
        if(c->phase==WORKER_CLIENT_HEADER && (now>=c->header_due || now>=c->input_idle_due))worker_http_error_response(reactor,slot,408,now);
        else if(c->phase==WORKER_CLIENT_BODY && (now>=c->body_due || now>=c->input_idle_due))worker_http_error_response(reactor,slot,408,now);
        else if(c->phase==WORKER_INTERIM_WRITE && now>=c->body_due){
            if(c->local_header_sent)worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);
            else worker_http_error_response(reactor,slot,408,now);
        }
        if(c->client_fd>=0 && client_output_pending(c)){
            if(!c->client_write_waiting){c->client_write_waiting=true;c->output_idle_due=now+IO_CALL_TIMEOUT_SECONDS*1000;}
            else if(c->output_idle_due && now>=c->output_idle_due)worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_CANCELLED);
        }
        if(c->phase==WORKER_CLIENT_DRAIN && now>=c->drain_due)worker_http_close_client(reactor,slot,now,BACKEND_OUTCOME_COMPLETE);
        if(worker_http_publish_completed(reactor,slot,now)<0)reactor->stopping=true;
        if(c->phase!=WORKER_SLOT_FREE && worker_http_update_interest(reactor,slot)<0)reactor->stopping=true;
    }
}
static int next_timeout(struct worker_http_reactor *reactor,uint64_t now,int cap){
    uint64_t due=UINT64_MAX;
#define TAKE(value) do{uint64_t d=(value);if(d && d<due)due=d;}while(0)
    for(size_t slot=0;slot<reactor->capacity;++slot){
        const struct worker_http_connection *c=&reactor->connections[slot];
        if(c->phase==WORKER_CLIENT_HEADER){TAKE(c->header_due);TAKE(c->input_idle_due);}
        if(c->phase==WORKER_CLIENT_BODY){TAKE(c->body_due);TAKE(c->input_idle_due);}
        if(c->phase==WORKER_INTERIM_WRITE)TAKE(c->body_due);
        if(c->phase==WORKER_BACKEND_SELECT){TAKE(c->select_due);TAKE(c->retry_at);}
        if(c->lifetime.phase==BACKEND_REQUEST_RELEASE_PENDING)TAKE(c->retry_at);
        if(c->phase>=WORKER_BACKEND_CONNECT && c->phase<=WORKER_BACKEND_BODY && !c->backend_finished)TAKE(c->backend_due);
        if(c->phase==WORKER_BACKEND_BODY && !c->input_paused && !c->backend_finished)TAKE(c->backend_idle_due);
        if(c->client_fd>=0 && client_output_pending(c))TAKE(c->output_idle_due);
        if(c->phase==WORKER_CLIENT_DRAIN)TAKE(c->drain_due);
        TAKE(worker_cgi_deadline(c));
    }
#undef TAKE
    int timeout=due==UINT64_MAX?-1:due<=now?0:due-now>INT_MAX?INT_MAX:(int)(due-now);
    if(cap>=0 && (timeout<0 || cap<timeout))timeout=cap;
    return timeout;
}
int create_worker_http_reactor(const struct proxy_config *config,struct shared_backend_memory *shared,
    uint32_t worker,uint64_t generation,size_t capacity,int channel,struct worker_http_reactor *reactor){
    if(!config || !shared || !shared->header || !reactor || !generation || channel<0 || !capacity ||
       capacity>INITIAL_WORKER_CONNECTION_LIMIT || worker>=shared->layout.worker_capacity ||
       (uint64_t)shared->layout.worker_capacity*capacity>shared->layout.reservation_capacity){errno=EINVAL;return -1;}
    if(reactor->connections){errno=EBUSY;return -1;}
    *reactor=(struct worker_http_reactor){.epoll_fd=-1,.channel_fd=channel,.config=config,.shared=shared,
        .worker_index=worker,.worker_generation=generation,.capacity=capacity};
    reactor->connections=calloc(capacity,sizeof(*reactor->connections));reactor->incoming=calloc(1,sizeof(*reactor->incoming));
    if(!reactor->connections || !reactor->incoming)goto error;
    for(size_t i=0;i<capacity;++i)reactor->connections[i].client_fd=reactor->connections[i].backend_fd=-1;
    if(create_worker_closed_queue(capacity,&reactor->completed)<0)goto error;
    reactor->epoll_fd=epoll_create1(EPOLL_CLOEXEC);if(reactor->epoll_fd<0)goto error;
    struct epoll_event event={.events=EPOLLIN,.data.u64=WORKER_CHANNEL_EVENT};
    if(epoll_ctl(reactor->epoll_fd,EPOLL_CTL_ADD,channel,&event)<0)goto error;
    if(create_worker_file_pool(capacity,&reactor->files)<0)goto error;
    event=(struct epoll_event){.events=EPOLLIN,.data.u64=WORKER_FILE_EVENT};
    if(epoll_ctl(reactor->epoll_fd,EPOLL_CTL_ADD,worker_file_notification_fd(reactor->files),&event)<0)goto error;
    return 0;
error:{int error=errno;destroy_worker_http_reactor(reactor);errno=error;return -1;}
}
int advance_worker_http_reactor(struct worker_http_reactor *reactor,struct process_signal_wait *signals,int timeout_cap){
    if(!reactor || !reactor->connections || !signals || timeout_cap< -1){errno=EINVAL;return -1;}
    reactor->signals=signals;
    if(reactor->stopping || process_signal_shutdown_requested(signals) || atomic_load(&reactor->shared->header->fatal))return 1;
    struct timespec time;uint64_t now;if(worker_http_now(&time,&now)<0)return -1;
    if(worker_file_service_completions(reactor,now)<0)return -1;
    worker_http_service_deadlines(reactor,now);if(reactor->stopping)return -1;
    if(channel_interest(reactor)<0)return -1;
    struct epoll_event events[WORKER_EPOLL_BATCH];
    int count=wait_process_signal_events(signals,reactor->epoll_fd,events,WORKER_EPOLL_BATCH,next_timeout(reactor,now,timeout_cap));
    if(process_signal_shutdown_requested(signals) || atomic_load(&reactor->shared->header->fatal))return 1;
    if(count<0 && errno!=EINTR)return -1;
    if(worker_http_now(&time,&now)<0)return -1;
    worker_http_service_deadlines(reactor,now);
    for(int i=0;i<count && !reactor->stopping;++i){
        uint64_t token=events[i].data.u64;
        if(token==WORKER_FILE_EVENT){if(drain_worker_file_notifications(reactor->files)<0 || worker_file_service_completions(reactor,now)<0)return -1;continue;}
        if(token==WORKER_CHANNEL_EVENT){if(advance_channel(reactor,events[i].events,now)<0)return -1;continue;}
        size_t slot=(uint32_t)token>>2;uint32_t generation=(uint32_t)(token>>32);
        if(slot>=reactor->capacity)continue;
        struct worker_http_connection *c=&reactor->connections[slot];if(c->phase==WORKER_SLOT_FREE || c->slot_generation!=generation)continue;
        if((token&3)==2){worker_cgi_advance(reactor,slot,now);}
        else if(token&1){struct worker_io_budget budget={0};worker_proxy_advance(reactor,slot,events[i].events,now,&budget);}
        else if(c->client_fd>=0)advance_client_event(reactor,slot,events[i].events,now);
    }
    if(worker_file_service_completions(reactor,now)<0)return -1;
    worker_http_service_deadlines(reactor,now);if(reactor->stopping)return -1;
    return channel_interest(reactor);
}
const struct http_request *worker_http_local_request(struct worker_http_reactor *reactor,size_t slot){
    if(!reactor || slot>=reactor->capacity || reactor->connections[slot].phase!=WORKER_LOCAL_READY)return NULL;
    return &reactor->connections[slot].request;
}
int resume_worker_http_memory_response(struct worker_http_reactor *reactor,size_t slot,int status,
    const char *type,const void *body,size_t length,const char *allow){
    if(!worker_http_local_request(reactor,slot) || !type || (!body && length)){errno=EINVAL;return -1;}
    struct timespec time;uint64_t now;if(worker_http_now(&time,&now)<0)return -1;
    if(status==500)reactor->connections[slot].internal_failure=true;
    if(prepare_memory_response(&reactor->connections[slot],status,type,body,length,allow,now)<0)return -1;
    return worker_http_update_interest(reactor,slot);
}
void destroy_worker_http_reactor(struct worker_http_reactor *reactor){
    if(!reactor)return;
    if(!reactor->config && !reactor->connections && !reactor->incoming){
        *reactor=(struct worker_http_reactor){.epoll_fd=-1,.channel_fd=-1};return;
    }
    destroy_worker_cgi_children(reactor);
    destroy_worker_file_pool(reactor->files);reactor->files=NULL;
    if(reactor->connections)for(size_t slot=0;slot<reactor->capacity;++slot){
        struct worker_http_connection *c=&reactor->connections[slot];
        if(c->phase!=WORKER_SLOT_FREE){if(c->client_fd>=0)close(c->client_fd);if(c->backend_fd>=0)close(c->backend_fd);free_connection_storage(c);}
    }
    if(reactor->incoming){abort_worker_fd_receive(reactor->incoming);free(reactor->incoming);}
    if(reactor->epoll_fd>=0)close(reactor->epoll_fd);
    destroy_worker_closed_queue(&reactor->completed);free(reactor->connections);
    *reactor=(struct worker_http_reactor){.epoll_fd=-1,.channel_fd=-1};
}
