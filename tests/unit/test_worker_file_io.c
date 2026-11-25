#define _GNU_SOURCE
#include "../../src/worker_file_io.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int gate[2],started[2];
static size_t fd_count(void){
    DIR *directory=opendir("/proc/self/fd");assert(directory);size_t count=0;struct dirent *entry;
    while((entry=readdir(directory)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))++count;
    assert(closedir(directory)==0);return count;
}
static void slow_file_handler(const struct http_request *request,struct router_response *response){
    assert(request->path.length==4 && !memcmp(request->path.data,"/one",4));
    assert(write(started[1],"S",1)==1);char release;assert(read(gate[0],&release,1)==1);
    router_prepare_error(response,404,NULL);
}
static void await_completion(struct worker_file_pool *pool,struct worker_file_job *job){
    for(;;){int ready=take_worker_file_completion(pool,job);assert(ready>=0);if(ready)return;
        struct pollfd event={worker_file_notification_fd(pool),POLLIN,0};assert(poll(&event,1,2000)>0);
        assert(drain_worker_file_notifications(pool)==0);}
}
int main(void){
    alarm(10);size_t baseline=fd_count();assert(pipe(gate)==0 && pipe(started)==0);
    struct worker_file_pool *pool=NULL;assert(create_worker_file_pool(2,&pool)==0);
    struct worker_file_job *first=NULL,*queued=NULL;
    const unsigned char path[]="/one";
    assert(start_worker_file_job(pool,0,slow_file_handler,(struct http_slice){path,4},&first)==0);
    struct pollfd began={started[0],POLLIN,0};assert(poll(&began,1,2000)>0);char marker;assert(read(started[0],&marker,1)==1);
    /* The disk helper is blocked, but caller can enqueue and continue control I/O. */
    assert(start_worker_file_job(pool,1,slow_file_handler,(struct http_slice){path,4},&queued)==0);
    assert(queue_worker_file_operation(pool,first,WORKER_FILE_READ)<0 && errno==EBUSY);
    assert(take_worker_file_completion(pool,first)==0 && take_worker_file_completion(pool,queued)==0);
    assert(write(gate[1],"RR",2)==2);
    await_completion(pool,first);assert(worker_file_response(first)->status==404);
    assert(queue_worker_file_operation(pool,first,WORKER_FILE_READ)==0);
    await_completion(pool,queued);assert(queue_worker_file_operation(pool,queued,WORKER_FILE_CLEANUP)==0);
    await_completion(pool,first);struct http_slice chunk;
    assert(worker_file_chunk(first,&chunk)==BODY_DATA && chunk.length==9 && !memcmp(chunk.data,"Not Found",9));
    assert(queue_worker_file_operation(pool,first,WORKER_FILE_CLEANUP)==0);
    await_completion(pool,queued);release_worker_file_job(pool,&queued);assert(!queued);
    await_completion(pool,first);release_worker_file_job(pool,&first);assert(!first);
    destroy_worker_file_pool(pool);close(gate[0]);close(gate[1]);close(started[0]);close(started[1]);
    assert(fd_count()==baseline);alarm(0);
    puts("[PASS] blocked file helper leaves caller responsive; bounded queue, one task/job, cancellation cleanup and FD baseline");return 0;
}
