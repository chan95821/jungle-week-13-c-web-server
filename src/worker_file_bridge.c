#define _GNU_SOURCE
#include "worker_http_internal.h"
#include "handlers/handlers.h"
#include "handlers/cgi_protocol.h"
#include <errno.h>
#include <sys/socket.h>
static int queue_file_operation(struct worker_http_reactor *reactor,struct worker_http_connection *c,enum worker_file_operation operation){
    if(queue_worker_file_operation(reactor->files,c->file_job,operation)<0){reactor->stopping=true;return -1;}
    c->file_waiting=true;return 0;
}
int worker_file_start_response(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    (void)now;struct worker_http_connection *c=&reactor->connections[slot];router_handler handler=c->route.local_handler;
    if(handler!=prepare_cafe && handler!=prepare_menu && handler!=prepare_assets && handler!=prepare_cgi)return 0;
    if(start_worker_file_request(reactor->files,slot,handler==prepare_cgi?prepare_cgi_launch:handler,&c->request,&c->file_job)<0)return -1;
    c->phase=handler==prepare_cgi?WORKER_CGI_PREPARE:WORKER_FILE_OUTPUT;c->file_waiting=true;return 0;
}
static void fail_file_response(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    struct worker_http_connection *c=&reactor->connections[slot];
    c->file_cancelled=true;c->memory_body=NULL;c->memory_length=c->memory_sent=0;
    worker_http_error_response(reactor,slot,500,now);
}
void worker_file_output_drained(struct worker_http_reactor *reactor,size_t slot,uint64_t now){
    (void)now;struct worker_http_connection *c=&reactor->connections[slot];
    if(!c->file_job){
        shutdown(c->client_fd,SHUT_WR);c->phase=WORKER_CLIENT_DRAIN;c->drain_due=now+DRAIN_DEADLINE_SECONDS*1000;return;
    }
    if(c->file_waiting)return;
    c->memory_body=NULL;c->memory_length=c->memory_sent=0;
    queue_file_operation(reactor,c,c->file_remaining?WORKER_FILE_READ:WORKER_FILE_CLEANUP);
}
int worker_file_service_completions(struct worker_http_reactor *reactor,uint64_t now){
    if(!reactor->files)return 0;
    for(size_t slot=0;slot<reactor->capacity;++slot){
        struct worker_http_connection *c=&reactor->connections[slot];if(!c->file_job)continue;
        if(c->file_waiting){
            int ready=take_worker_file_completion(reactor->files,c->file_job);if(ready<0)return -1;if(!ready)continue;
            c->file_waiting=false;
            enum worker_file_operation operation=worker_file_last_operation(c->file_job);
            if(operation==WORKER_FILE_CLEANUP){
                release_worker_file_job(reactor->files,&c->file_job);
                if(c->file_job){errno=EPROTO;return -1;}
                if(c->phase==WORKER_FILE_OUTPUT && !c->file_cancelled && c->local_header_sent==c->local_header_length && c->memory_sent==c->memory_length){
                    shutdown(c->client_fd,SHUT_WR);c->phase=WORKER_CLIENT_DRAIN;c->drain_due=now+DRAIN_DEADLINE_SECONDS*1000;
                }
                continue;
            }
            if(c->phase!=WORKER_CLIENT_CLOSED && !c->file_cancelled){
                if(operation==WORKER_FILE_PREPARE){
                    const struct router_response *response=worker_file_response(c->file_job);
                    if(c->phase==WORKER_CGI_PREPARE){
                        c->file_cancelled=true; /* Launch metadata cleanup, not CGI cancellation. */
                        if(response->status!=200)worker_http_error_response(reactor,slot,response->status,now);
                        else if(worker_cgi_start(reactor,slot,response->body_provider.state,now)<0)
                            worker_http_error_response(reactor,slot,500,now);
                        if(queue_file_operation(reactor,c,WORKER_FILE_CLEANUP)<0)return -1;
                        if(worker_http_update_interest(reactor,slot)<0)return -1;
                        continue;
                    }
                    c->file_remaining=response->length;
                    if(worker_http_prepare_file_headers(reactor,slot,response,now)<0)fail_file_response(reactor,slot,now);
                }else{
                    struct http_slice chunk;
                    if(worker_file_chunk(c->file_job,&chunk)!=BODY_DATA || !chunk.data || !chunk.length)
                        fail_file_response(reactor,slot,now);
                    else{
                        size_t size=chunk.length<c->file_remaining?chunk.length:c->file_remaining;
                        c->memory_body=chunk.data;c->memory_length=size;c->memory_sent=0;c->file_remaining-=size;
                    }
                }
            }
        }
        if(c->phase==WORKER_CLIENT_CLOSED || c->file_cancelled){
            if(!c->file_waiting && queue_file_operation(reactor,c,WORKER_FILE_CLEANUP)<0)return -1;
        }else if(c->phase==WORKER_FILE_OUTPUT && !c->file_waiting && c->memory_sent==c->memory_length){
            /* Headers may still be pending: at most one read-ahead chunk. */
            if(queue_file_operation(reactor,c,c->file_remaining?WORKER_FILE_READ:WORKER_FILE_CLEANUP)<0)return -1;
        }
        if(worker_http_update_interest(reactor,slot)<0)return -1;
    }
    return 0;
}
