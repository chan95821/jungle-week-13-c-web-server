#ifndef WORKER_FILE_IO_H
#define WORKER_FILE_IO_H
#include "router.h"
struct worker_file_pool;
struct worker_file_job;
enum worker_file_operation { WORKER_FILE_PREPARE, WORKER_FILE_READ, WORKER_FILE_CLEANUP };
/* One inherited worker process owns this bounded pool, created AFTER fork.
 * A single helper owns all providers; caller owns sockets and slot lifetime. */
int create_worker_file_pool(size_t capacity,struct worker_file_pool **pool);
int worker_file_notification_fd(const struct worker_file_pool *pool);
int drain_worker_file_notifications(struct worker_file_pool *pool);
int start_worker_file_job(struct worker_file_pool *pool,size_t slot,router_handler handler,
    struct http_slice path,struct worker_file_job **job);
int start_worker_file_request(struct worker_file_pool *pool,size_t slot,router_handler handler,const struct http_request *request,struct worker_file_job **job);
int queue_worker_file_operation(struct worker_file_pool *pool,struct worker_file_job *job,
    enum worker_file_operation operation);
/* 1 completion acquired, 0 still running, -1 invalid. All result views remain
 * stable until the next operation is queued. No provider invoked by caller. */
int take_worker_file_completion(struct worker_file_pool *pool,struct worker_file_job *job);
enum worker_file_operation worker_file_last_operation(const struct worker_file_job *job);
const struct router_response *worker_file_response(const struct worker_file_job *job);
enum body_result worker_file_chunk(const struct worker_file_job *job,struct http_slice *chunk);
void release_worker_file_job(struct worker_file_pool *pool,struct worker_file_job **job);
/* Stop accepting work; join helper after current disk call, clean all providers.
 * A stuck disk call is not forcibly pthread_cancel'ed; parent supervisor bounds
 * process shutdown. Caller must not keep using jobs after destroy. */
void destroy_worker_file_pool(struct worker_file_pool *pool);
#endif
