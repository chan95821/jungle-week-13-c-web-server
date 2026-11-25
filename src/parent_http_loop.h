#ifndef PARENT_HTTP_LOOP_H
#define PARENT_HTTP_LOOP_H
#include "parent_worker_supervisor.h"
#include "process_signal_wait.h"
#include "proxy.h"
#include "worker_closed_channel.h"
struct parent_http_channel {
    struct worker_closed_receiver completed;
    uint64_t sending_connection, generation;
    int registered_fd;
    uint32_t interest;
    bool available;
};
/* Create before fork; parent owns epoll and channel bookkeeping, borrows all
 * runtime owners/FDs. Child closes inherited epoll_fd before its reactor starts.
 * No worker creation, HTTP parsing, backend selection or blocking client I/O. */
struct parent_http_loop {
    int epoll_fd, listener_fd, failure_fd;
    uint32_t listener_interest;
    struct parent_http_channel *channels;
    bool *available;
    struct parent_worker_supervisor *supervisor;
    const struct proxy_config *config;
    struct process_signal_wait *signals;
};
int create_parent_http_loop(int listener_fd,int failure_read_fd,
    struct parent_worker_supervisor *supervisor,const struct proxy_config *config,
    struct process_signal_wait *signals,struct parent_http_loop *loop);
/* Called after all workers have been adopted and startup finished. Runs until
 * shutdown completes. 0 requested clean exit, 1 runtime error, -1 bad arguments.
 * Owns no stderr/log sink policy. Bounded supervisor diagnostics remain inspectable.
 * Caller destroys shared mapping only after all children are confirmed reaped. */
int run_parent_http_loop(struct parent_http_loop *loop);
void destroy_parent_http_loop(struct parent_http_loop *loop);
#endif
