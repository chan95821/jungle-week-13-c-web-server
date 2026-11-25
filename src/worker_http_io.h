#ifndef WORKER_HTTP_IO_H
#define WORKER_HTTP_IO_H
#include "proxy.h"
#include "shared_backend_state.h"
#include "process_signal_wait.h"
#include "worker_closed_channel.h"
struct worker_http_connection;
struct worker_file_pool;
/* Single worker owns stable slots, epoll, client/backend FDs and buffers.
 * Borrows immutable config, its inherited shared view and worker IPC FD.
 * The IPC owner closes it after reactor destruction / process exit. */
struct worker_http_reactor {
    struct worker_http_connection *connections;
    size_t capacity, active;
    uint32_t worker_index;
    uint64_t worker_generation;
    int epoll_fd, channel_fd;
    bool stopping;
    struct shared_backend_memory *shared;
    const struct proxy_config *config;
    struct worker_closed_queue completed;
    struct backend_state_diagnostic diagnostic;
    struct worker_fd_receiver *incoming;
    struct worker_file_pool *files;
    struct process_signal_wait *signals;
};
/* Zero-initialize reactor. Call after fork/unused inherited FDs closed and shared generation activated.
 * capacity bounded by shared reservation slots and the approved C256 limit. */
int create_worker_http_reactor(const struct proxy_config *config,
    struct shared_backend_memory *shared,uint32_t worker,uint64_t generation,
    size_t capacity,int channel_fd,struct worker_http_reactor *reactor);
/* One actual LT epoll_pwait turn. 0 progress, 1 shutdown requested, -1 error.
 * Managed masks prepared by caller. Helper created after worker fork; CGI child only execs async-signal-safe setup.
 * timeout_cap_ms -1 unlimited, otherwise cap (test/integration control). */
int advance_worker_http_reactor(struct worker_http_reactor *reactor,
    struct process_signal_wait *signals,int timeout_cap_ms);
/* Unintegrated local route is suspended here; memory/file/CGI routes are driven internally.
 * External local I/O design must prepare bytes at stable address until CLOSED.
 * Index outputs are process-local slot indexes, not client connection IDs. */
const struct http_request *worker_http_local_request(struct worker_http_reactor *reactor,size_t slot);
int resume_worker_http_memory_response(struct worker_http_reactor *reactor,size_t slot,
    int status,const char *type,const void *body,size_t length,const char *allow);
/* Exit boundary only. Close all owned FDs/free buffers; unreleased shared
 * reservations remain for parent's waitpid-confirmed generation reclaim.
 * Do not resume this worker generation after destroy. IPC FD is borrowed. */
void destroy_worker_http_reactor(struct worker_http_reactor *reactor);
#endif
