#define _GNU_SOURCE
#include "worker_file_io.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct worker_file_job {
    size_t slot;
    router_handler handler;
    unsigned char *path;
    size_t path_length;
    unsigned char *query;
    size_t query_length;
    struct router_response response;
    struct http_slice chunk;
    enum worker_file_operation operation;
    enum body_result result;
    enum { FILE_IDLE, FILE_QUEUED, FILE_RUNNING, FILE_DONE } phase;
    bool cleaned;
};
struct worker_file_pool {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    struct worker_file_job **jobs,**queue;
    size_t capacity,head,count;
    int notification[2];
    bool stopping;
};
static void cleanup_provider(struct worker_file_job *job){
    if(job->response.body_provider.cleanup_func)
        job->response.body_provider.cleanup_func(job->response.body_provider.state);
    job->response=(struct router_response){0};job->chunk=(struct http_slice){0};job->cleaned=true;
}
static void execute_file_operation(struct worker_file_job *job){
    if(job->operation==WORKER_FILE_PREPARE){
        struct http_request request={.path={job->path,job->path_length},.query={job->query,job->query_length}};
        job->handler(&request,&job->response);
    }else if(job->operation==WORKER_FILE_READ){
        job->chunk=(struct http_slice){0};
        job->result=job->response.body_provider.next_func?
            job->response.body_provider.next_func(job->response.body_provider.state,&job->chunk):BODY_ERROR;
    }else cleanup_provider(job);
}
static void *run_file_helper(void *argument){
    struct worker_file_pool *pool=argument;
    pthread_mutex_lock(&pool->mutex);
    for(;;){
        while(!pool->count && !pool->stopping)pthread_cond_wait(&pool->ready,&pool->mutex);
        if(pool->stopping)break;
        struct worker_file_job *job=pool->queue[pool->head];
        pool->head=(pool->head+1)%pool->capacity;--pool->count;job->phase=FILE_RUNNING;
        pthread_mutex_unlock(&pool->mutex);
        execute_file_operation(job);
        pthread_mutex_lock(&pool->mutex);job->phase=FILE_DONE;pthread_mutex_unlock(&pool->mutex);
        /* Coalesced nonblocking hint; DONE under mutex is authoritative. */
        ssize_t sent;do{sent=write(pool->notification[1],"F",1);}while(sent<0 && errno==EINTR);
        pthread_mutex_lock(&pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
    for(size_t slot=0;slot<pool->capacity;++slot)if(pool->jobs[slot])cleanup_provider(pool->jobs[slot]);
    return NULL;
}
int create_worker_file_pool(size_t capacity,struct worker_file_pool **out){
    if(!out || *out || !capacity){errno=EINVAL;return -1;}
    struct worker_file_pool *pool=calloc(1,sizeof(*pool));if(!pool)return -1;
    pool->capacity=capacity;pool->notification[0]=pool->notification[1]=-1;
    pool->jobs=calloc(capacity,sizeof(*pool->jobs));pool->queue=calloc(capacity,sizeof(*pool->queue));
    if(!pool->jobs || !pool->queue)goto fail_alloc;
    int error=pthread_mutex_init(&pool->mutex,NULL);if(error){errno=error;goto fail_alloc;}
    error=pthread_cond_init(&pool->ready,NULL);if(error){errno=error;goto fail_mutex;}
    if(pipe2(pool->notification,O_NONBLOCK|O_CLOEXEC)<0)goto fail_cond;
    sigset_t blocked,prior;sigemptyset(&blocked);sigaddset(&blocked,SIGINT);sigaddset(&blocked,SIGTERM);sigaddset(&blocked,SIGCHLD);
    error=pthread_sigmask(SIG_BLOCK,&blocked,&prior);if(error){errno=error;goto fail_pipe;}
    error=pthread_create(&pool->thread,NULL,run_file_helper,pool);
    int restored=pthread_sigmask(SIG_SETMASK,&prior,NULL);
    if(error){errno=error;goto fail_pipe;}
    if(restored){pthread_mutex_lock(&pool->mutex);pool->stopping=true;pthread_cond_signal(&pool->ready);
        pthread_mutex_unlock(&pool->mutex);pthread_join(pool->thread,NULL);errno=restored;goto fail_pipe;}
    *out=pool;return 0;
fail_pipe:{int saved=errno;close(pool->notification[0]);close(pool->notification[1]);errno=saved;}
fail_cond:{int saved=errno;pthread_cond_destroy(&pool->ready);errno=saved;}
fail_mutex:{int saved=errno;pthread_mutex_destroy(&pool->mutex);errno=saved;}
fail_alloc:{int saved=errno;free(pool->jobs);free(pool->queue);free(pool);errno=saved;return -1;}
}
int worker_file_notification_fd(const struct worker_file_pool *pool){return pool?pool->notification[0]:-1;}
int drain_worker_file_notifications(struct worker_file_pool *pool){
    unsigned char bytes[256];
    for(;;){ssize_t count=read(pool->notification[0],bytes,sizeof(bytes));
        if(count>0)continue;
        if(count<0 && errno==EINTR)continue;
        if(count<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
        errno=EPIPE;return -1;}
}
int queue_worker_file_operation(struct worker_file_pool *pool,struct worker_file_job *job,enum worker_file_operation op){
    if(!pool || !job || op<WORKER_FILE_PREPARE || op>WORKER_FILE_CLEANUP){errno=EINVAL;return -1;}
    pthread_mutex_lock(&pool->mutex);
    if(job->slot>=pool->capacity || pool->jobs[job->slot]!=job || pool->stopping || job->phase!=FILE_IDLE || pool->count==pool->capacity || job->cleaned){
        pthread_mutex_unlock(&pool->mutex);errno=EBUSY;return -1;}
    job->operation=op;job->phase=FILE_QUEUED;
    pool->queue[(pool->head+pool->count)%pool->capacity]=job;++pool->count;
    pthread_cond_signal(&pool->ready);pthread_mutex_unlock(&pool->mutex);return 0;
}
static int start_file_request(struct worker_file_pool *pool,size_t slot,router_handler handler,
    struct http_slice path,struct http_slice query,struct worker_file_job **out){
    if(!pool || !out || *out || !handler || slot>=pool->capacity || (!path.data && path.length)){errno=EINVAL;return -1;}
    struct worker_file_job *job=calloc(1,sizeof(*job));if(!job)return -1;
    job->path=malloc(path.length?path.length:1);if(!job->path){free(job);return -1;}
    if(path.length)memcpy(job->path,path.data,path.length);
    job->query=malloc(query.length?query.length:1);if(!job->query){free(job->query);free(job->path);free(job);return -1;}
    if(query.length)memcpy(job->query,query.data,query.length);
    job->query_length=query.length;job->path_length=path.length;job->slot=slot;job->handler=handler;
    pthread_mutex_lock(&pool->mutex);
    if(pool->jobs[slot]){pthread_mutex_unlock(&pool->mutex);free(job->query);free(job->path);free(job);errno=EBUSY;return -1;}
    pool->jobs[slot]=job;pthread_mutex_unlock(&pool->mutex);
    if(queue_worker_file_operation(pool,job,WORKER_FILE_PREPARE)<0){
        pthread_mutex_lock(&pool->mutex);pool->jobs[slot]=NULL;pthread_mutex_unlock(&pool->mutex);
        free(job->query);free(job->path);free(job);return -1;}
    *out=job;return 0;
}
int start_worker_file_job(struct worker_file_pool *pool,size_t slot,router_handler handler,struct http_slice path,struct worker_file_job **out){
    return start_file_request(pool,slot,handler,path,(struct http_slice){0},out);
}
int start_worker_file_request(struct worker_file_pool *pool,size_t slot,router_handler handler,const struct http_request *request,struct worker_file_job **out){
    return start_file_request(pool,slot,handler,request->path,request->query,out);
}
int take_worker_file_completion(struct worker_file_pool *pool,struct worker_file_job *job){
    if(!pool || !job){errno=EINVAL;return -1;}
    pthread_mutex_lock(&pool->mutex);int done=job->phase==FILE_DONE;
    if(done)job->phase=FILE_IDLE;
    pthread_mutex_unlock(&pool->mutex);return done;
}
enum worker_file_operation worker_file_last_operation(const struct worker_file_job *job){return job->operation;}
const struct router_response *worker_file_response(const struct worker_file_job *job){return &job->response;}
enum body_result worker_file_chunk(const struct worker_file_job *job,struct http_slice *chunk){*chunk=job->chunk;return job->result;}
void release_worker_file_job(struct worker_file_pool *pool,struct worker_file_job **out){
    if(!pool || !out || !*out)return;
    struct worker_file_job *job=*out;
    pthread_mutex_lock(&pool->mutex);
    if(job->phase!=FILE_IDLE || !job->cleaned){pthread_mutex_unlock(&pool->mutex);return;}
    pool->jobs[job->slot]=NULL;pthread_mutex_unlock(&pool->mutex);
    free(job->query);free(job->path);free(job);*out=NULL;
}
void destroy_worker_file_pool(struct worker_file_pool *pool){
    if(!pool)return;
    pthread_mutex_lock(&pool->mutex);pool->stopping=true;pthread_cond_signal(&pool->ready);pthread_mutex_unlock(&pool->mutex);
    pthread_join(pool->thread,NULL);
    for(size_t slot=0;slot<pool->capacity;++slot)if(pool->jobs[slot]){free(pool->jobs[slot]->query);free(pool->jobs[slot]->path);free(pool->jobs[slot]);}
    close(pool->notification[0]);close(pool->notification[1]);pthread_cond_destroy(&pool->ready);pthread_mutex_destroy(&pool->mutex);
    free(pool->queue);free(pool->jobs);free(pool);
}
