#define _GNU_SOURCE
#include "worker_http_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool transient_error(void){return errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK;}
static void backend_failure(struct worker_http_reactor *reactor,size_t slot,int status,uint64_t now){
    if(worker_http_error_response(reactor,slot,status,now)<0)reactor->stopping=true;
}
static void compact_relay(struct worker_http_connection *c){
    size_t used=worker_http_relay_used(c);
    if(c->relay_offset && used)memmove(c->response->buffer,c->response->buffer+c->relay_offset,used);
    c->relay_offset=0;c->relay_length=used;
}
static void pause_backend_input(struct worker_http_connection *c,uint64_t now){
    if(c->input_paused)return;
    c->input_paused=true;c->paused_idle_ms=c->backend_idle_due>now?c->backend_idle_due-now:0;
}
static size_t relay_payload_capacity(struct worker_http_connection *c){
    size_t space=WORKER_RELAY_BYTES-worker_http_relay_used(c);
    if(c->response->framing==BACKEND_LENGTH)return space;
    if(space<6)return 0;
    size_t amount=space-5;
    for(;;){char prefix[32];int length=snprintf(prefix,sizeof(prefix),"%zx\r\n",amount);
        if(length<0)return 0;
        if((size_t)length+amount+2<=space)return amount;
        amount=space-(size_t)length-2;
    }
}
static void append_relay_data(struct worker_http_connection *c,const unsigned char *data,size_t length){
    compact_relay(c);
    if(c->response->framing==BACKEND_LENGTH){memcpy(c->response->buffer+c->relay_length,data,length);c->relay_length+=length;return;}
    char prefix[32];int count=snprintf(prefix,sizeof(prefix),"%zx\r\n",length);
    memcpy(c->response->buffer+c->relay_length,prefix,(size_t)count);c->relay_length+=(size_t)count;
    memcpy(c->response->buffer+c->relay_length,data,length);c->relay_length+=length;
    memcpy(c->response->buffer+c->relay_length,"\r\n",2);c->relay_length+=2;
}
static int read_backend_bytes(struct worker_http_connection *c,void *buffer,size_t length,
    uint64_t now,struct worker_io_budget *budget,ssize_t *received){
    if(budget->calls>=WORKER_EVENT_CALLS || budget->bytes>=WORKER_EVENT_BYTES)return 0;
    if(length>WORKER_EVENT_BYTES-budget->bytes)length=WORKER_EVENT_BYTES-budget->bytes;
    ++budget->calls;*received=recv(c->backend_fd,buffer,length,MSG_DONTWAIT);
    if(*received<0)return transient_error()?0:-1;
    if(*received>0){budget->bytes+=(size_t)*received;c->backend_idle_due=now+PROXY_BODY_IDLE_SECONDS*1000;}
    return 1;
}
static void begin_backend_connection(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    c->response=calloc(1,sizeof(*c->response));c->headers=calloc(1,sizeof(*c->headers));
    if(!c->response || !c->headers){backend_failure(reactor,slot,500,now);return;}
    c->response->backend_fd=-1;
    if(build_backend_request_headers(c->headers->client_header,sizeof(c->headers->client_header),
        &c->backend_request_length,c->backend_address,&c->request)<0){backend_failure(reactor,slot,500,now);return;}
    c->backend_fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(c->backend_fd<0){backend_failure(reactor,slot,502,now);return;}
    struct timespec time;uint64_t ignored;
    if(worker_http_now(&time,&ignored)<0 || start_backend_request_clock(&c->lifetime,&time)<0){backend_failure(reactor,slot,500,now);return;}
    c->backend_due=now+PROXY_CONNECT_SECONDS*1000;c->phase=WORKER_BACKEND_CONNECT;
    if(connect(c->backend_fd,(const struct sockaddr *)c->backend_address,sizeof(*c->backend_address))<0){
        if(errno!=EINPROGRESS && errno!=EINTR){backend_failure(reactor,slot,502,now);return;}
    }else {c->phase=WORKER_BACKEND_REQUEST;c->backend_due=now+PROXY_REQUEST_SECONDS*1000;}
}
static void select_backend(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    if(now>=c->select_due){backend_failure(reactor,slot,503,now);return;}
    if(now<c->retry_at)return;
    if(try_lock_shared_backend_state(reactor->shared,&reactor->diagnostic)<0){
        if(errno==EBUSY){c->retry_at=now+INITIAL_WORKER_LOCK_RETRY_MS;return;}
        reactor->stopping=true;return;
    }
    uint32_t reservation=(uint32_t)(reactor->worker_index*reactor->capacity+slot),backend=0;
    int result;
    if(c->route.proxy_route==PROXY_ROUTE_QUERY)
        result=begin_selected_query_request_locked(reactor->shared,&c->lifetime,reactor->config->query_policy,
            reactor->worker_index,reactor->worker_generation,reservation,&backend,&reactor->diagnostic);
    else result=begin_backend_request_locked(reactor->shared,&c->lifetime,reactor->worker_index,
            reactor->worker_generation,reservation,0,reactor->shared->header->next_query_turn,&reactor->diagnostic);
    int error=errno;
    if(unlock_shared_backend_state(reactor->shared)<0){reactor->stopping=true;return;}
    if(result<0){errno=error;if(error==EUCLEAN || atomic_load(&reactor->shared->header->fatal))reactor->stopping=true;
        else backend_failure(reactor,slot,500,now);
        return;}
    c->retry_at=0;c->backend_address=backend?&reactor->config->query_backends[backend-1]:&reactor->config->submission_backend;
    begin_backend_connection(reactor,slot,now);
}
static void write_backend_request(struct worker_http_reactor *reactor,size_t slot,uint64_t now,struct worker_io_budget *budget){
    struct worker_http_connection *c=&reactor->connections[slot];
    size_t total=c->backend_request_length+c->request.body.length;
    while(c->request_sent<total && budget->calls<WORKER_EVENT_CALLS && budget->bytes<WORKER_EVENT_BYTES){
        const unsigned char *data;size_t length;
        if(c->request_sent<c->backend_request_length){data=c->headers->client_header+c->request_sent;length=c->backend_request_length-c->request_sent;}
        else {size_t sent=c->request_sent-c->backend_request_length;data=c->request.body.data+sent;length=c->request.body.length-sent;}
        if(length>WORKER_EVENT_BYTES-budget->bytes)length=WORKER_EVENT_BYTES-budget->bytes;
        ++budget->calls;ssize_t count=send(c->backend_fd,data,length,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(count<0){if(transient_error())return;backend_failure(reactor,slot,502,now);return;}
        if(!count){backend_failure(reactor,slot,502,now);return;}
        c->request_sent+=(size_t)count;budget->bytes+=(size_t)count;
    }
    if(c->request_sent==total){memset(c->headers,0,sizeof(*c->headers));c->phase=WORKER_BACKEND_HEADER;c->backend_due=now+PROXY_HEADER_SECONDS*1000;}
}
static void finish_backend_headers(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];struct proxy_stream *stream=c->response;
    if(parse_backend_headers(stream,c->headers)<0 || stream->status==101){backend_failure(reactor,slot,502,now);return;}
    if(stream->status<200){
        if(stream->status>100 && build_client_response_headers(stream,c->headers)<0){backend_failure(reactor,slot,502,now);return;}
        c->headers->raw_header_length=0;return;
    }
    if(build_client_response_headers(stream,c->headers)<0 || retain_trailer_connection_fields(stream,c->headers->fields)<0){backend_failure(reactor,slot,502,now);return;}
    stream->remaining=stream->content_length;c->chunk_phase=WORKER_CHUNK_SIZE;
    c->phase=WORKER_BACKEND_BODY;c->backend_due=now+PROXY_BODY_SECONDS*1000;
    c->backend_idle_due=now+PROXY_BODY_IDLE_SECONDS*1000;c->chunk_line_used=0;
    if(stream->framing==BACKEND_NO_BODY || (stream->framing==BACKEND_LENGTH && !stream->remaining))
        worker_http_backend_finished(reactor,slot,now,BACKEND_OUTCOME_COMPLETE);
}
static void read_backend_headers(struct worker_http_reactor *reactor,size_t slot,uint64_t now,struct worker_io_budget *budget){
    struct worker_http_connection *c=&reactor->connections[slot];
    while(c->phase==WORKER_BACKEND_HEADER && budget->calls<WORKER_EVENT_CALLS && budget->bytes<WORKER_EVENT_BYTES){
        if(c->headers->raw_header_length==sizeof(c->headers->raw_header)){backend_failure(reactor,slot,502,now);return;}
        unsigned char byte;ssize_t count;int result=read_backend_bytes(c,&byte,1,now,budget,&count);
        if(!result)return;
        if(result<0 || !count){backend_failure(reactor,slot,502,now);return;}
        c->headers->raw_header[c->headers->raw_header_length++]=byte;size_t used=c->headers->raw_header_length;
        if(used>=4 && !memcmp(c->headers->raw_header+used-4,"\r\n\r\n",4))finish_backend_headers(reactor,slot,now);
    }
}
static bool forbidden_trailer(struct worker_http_connection *c,struct http_slice name){
    static const char *const forbidden[]={"content-length","host","trailer","content-type","content-encoding","content-range"};
    if(proxy_header_is_hop_by_hop((struct http_slice){c->response->connection_fields,c->response->connection_fields_length},name))return true;
    for(size_t i=0;i<sizeof(forbidden)/sizeof(forbidden[0]);++i)if(http_field_name_equal(name,forbidden[i]))return true;
    return false;
}
static int complete_chunk_line(struct worker_http_connection *c){
    size_t length=c->chunk_line_used;unsigned char *line=c->headers->raw_header;
    if(length<2 || memcmp(line+length-2,"\r\n",2))return 0;
    if(c->chunk_phase==WORKER_CHUNK_SIZE){
        if(parse_chunk_size((struct http_slice){line,length-2},&c->response->chunk_remaining)<0)return -1;
        c->chunk_phase=c->response->chunk_remaining?WORKER_CHUNK_DATA:WORKER_CHUNK_TRAILERS;
    }else {
        if(length==2){c->response->body_complete=true;c->chunk_line_used=0;return 1;}
        size_t offset=0;struct http_field field;
        if(http_next_field((struct http_slice){line,length},&offset,&field)!=1 || forbidden_trailer(c,field.name))return -1;
        if(length>REQ_HEADER_MAX_BYTES-c->trailer_used)return -1;
        if(!c->response->trailer_buffer){c->response->trailer_buffer=malloc(REQ_HEADER_MAX_BYTES);if(!c->response->trailer_buffer)return -1;}
        memcpy(c->response->trailer_buffer+c->trailer_used,line,length);c->trailer_used+=length;
    }
    c->chunk_line_used=0;return 0;
}
static void append_final_chunk(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    size_t required=5+c->trailer_used;
    if(required>WORKER_RELAY_BYTES-worker_http_relay_used(c)){
        c->terminator_pending=true;
        if(!c->backend_finished)worker_http_backend_finished(reactor,slot,now,BACKEND_OUTCOME_COMPLETE);
        return;
    }
    compact_relay(c);memcpy(c->response->buffer+c->relay_length,"0\r\n",3);c->relay_length+=3;
    if(c->trailer_used){memcpy(c->response->buffer+c->relay_length,c->response->trailer_buffer,c->trailer_used);c->relay_length+=c->trailer_used;}
    memcpy(c->response->buffer+c->relay_length,"\r\n",2);c->relay_length+=2;
    c->terminator_pending=false;
    if(!c->backend_finished)worker_http_backend_finished(reactor,slot,now,BACKEND_OUTCOME_COMPLETE);
}
static void read_backend_body(struct worker_http_reactor *reactor,size_t slot,uint64_t now,struct worker_io_budget *budget){
    struct worker_http_connection *c=&reactor->connections[slot];struct proxy_stream *stream=c->response;
    unsigned char input[WORKER_RELAY_BYTES]; /* One bounded transient worker stack buffer, not one heap per connection. */
    while(!c->backend_finished && !c->input_paused && budget->calls<WORKER_EVENT_CALLS && budget->bytes<WORKER_EVENT_BYTES){
        if(stream->body_complete){append_final_chunk(reactor,slot,now);return;}
        if(stream->framing==BACKEND_CHUNKED && c->chunk_phase!=WORKER_CHUNK_DATA){
            unsigned char byte;ssize_t count;int result=read_backend_bytes(c,&byte,1,now,budget,&count);
            if(!result)return;
            if(result<0 || !count){backend_failure(reactor,slot,502,now);return;}
            if(c->chunk_phase==WORKER_CHUNK_CR){if(byte!='\r'){backend_failure(reactor,slot,502,now);return;}c->chunk_phase=WORKER_CHUNK_LF;continue;}
            if(c->chunk_phase==WORKER_CHUNK_LF){if(byte!='\n'){backend_failure(reactor,slot,502,now);return;}c->chunk_phase=WORKER_CHUNK_SIZE;continue;}
            if(c->chunk_line_used==sizeof(c->headers->raw_header)){backend_failure(reactor,slot,502,now);return;}
            c->headers->raw_header[c->chunk_line_used++]=byte;
            int parsed=complete_chunk_line(c);if(parsed<0){backend_failure(reactor,slot,502,now);return;}
            if(parsed==1){append_final_chunk(reactor,slot,now);return;}
            continue;
        }
        size_t capacity=relay_payload_capacity(c);
        if(!capacity){pause_backend_input(c,now);return;}
        if(stream->framing==BACKEND_LENGTH && capacity>stream->remaining)capacity=stream->remaining;
        if(stream->framing==BACKEND_CHUNKED && capacity>stream->chunk_remaining)capacity=stream->chunk_remaining;
        if(capacity>sizeof(input))capacity=sizeof(input);
        ssize_t count;int result=read_backend_bytes(c,input,capacity,now,budget,&count);
        if(!result)return;
        if(result<0){backend_failure(reactor,slot,502,now);return;}
        if(!count){
            if(stream->framing!=BACKEND_CLOSE){backend_failure(reactor,slot,502,now);return;}
            stream->body_complete=true;append_final_chunk(reactor,slot,now);return;
        }
        if(stream->status==205){backend_failure(reactor,slot,502,now);return;}
        append_relay_data(c,input,(size_t)count);
        if(stream->framing==BACKEND_LENGTH){stream->remaining-=(size_t)count;if(!stream->remaining){worker_http_backend_finished(reactor,slot,now,BACKEND_OUTCOME_COMPLETE);return;}}
        else if(stream->framing==BACKEND_CHUNKED){stream->chunk_remaining-=(size_t)count;if(!stream->chunk_remaining)c->chunk_phase=WORKER_CHUNK_CR;}
        if(!relay_payload_capacity(c)){pause_backend_input(c,now);return;}
    }
}
void worker_proxy_advance(struct worker_http_reactor *reactor,size_t slot,uint32_t events,uint64_t now,struct worker_io_budget *budget){
    struct worker_http_connection *c=&reactor->connections[slot];if(c->backend_fd<0)return;
    if(c->phase==WORKER_BACKEND_CONNECT){
        int error=0;socklen_t length=sizeof(error);++budget->calls;
        if(getsockopt(c->backend_fd,SOL_SOCKET,SO_ERROR,&error,&length)<0 || error){backend_failure(reactor,slot,502,now);return;}
        c->phase=WORKER_BACKEND_REQUEST;c->backend_due=now+PROXY_REQUEST_SECONDS*1000;
    }else if(events&EPOLLERR){backend_failure(reactor,slot,502,now);return;}
    if(c->phase==WORKER_BACKEND_REQUEST)write_backend_request(reactor,slot,now,budget);
    if(c->phase==WORKER_BACKEND_HEADER)read_backend_headers(reactor,slot,now,budget);
    if(c->phase==WORKER_BACKEND_BODY && !c->backend_finished && !c->input_paused)read_backend_body(reactor,slot,now,budget);
}
void worker_proxy_timer(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    if(c->lifetime.phase==BACKEND_REQUEST_RELEASE_PENDING && now>=c->retry_at){
        int result=try_release_backend_request(reactor->shared,&c->lifetime,&reactor->diagnostic);
        if(result==1)c->retry_at=now+INITIAL_WORKER_LOCK_RETRY_MS;
        else if(result<0)reactor->stopping=true;
        else c->retry_at=0;
    }
    if(c->phase==WORKER_BACKEND_SELECT)select_backend(reactor,slot,now);
    if(c->backend_fd>=0 && now>=c->backend_due){backend_failure(reactor,slot,504,now);return;}
    if(c->phase==WORKER_BACKEND_BODY && !c->backend_finished && !c->input_paused && now>=c->backend_idle_due){backend_failure(reactor,slot,504,now);return;}
    /* A final 0 chunk waiting for relay space has no further readable backend
     * data to create a new event: finish it from normal loop progress too. */
    if(c->phase==WORKER_BACKEND_BODY && c->response->body_complete && c->terminator_pending)
        append_final_chunk(reactor,slot,now);
}
