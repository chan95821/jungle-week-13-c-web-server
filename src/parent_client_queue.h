#ifndef PARENT_CLIENT_QUEUE_H
#define PARENT_CLIENT_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct parent_queued_client {
    int client_fd;
    uint64_t connection_id;
    struct timespec accepted_at;
};

/* Parent-only fixed ring. Zero-initialize; neither shared nor thread-safe. */
struct parent_client_queue {
    struct parent_queued_client *clients;
    size_t capacity;
    size_t head;
    size_t count;
};

int create_parent_client_queue(size_t capacity, struct parent_client_queue *queue);
/* The parent loop must use this gate before accept. It must keep servicing
 * existing clients/control and re-evaluate after queue/global count changes.
 * This helper does not change epoll interest or own the global ledger. */
bool parent_accept_has_capacity(const struct parent_client_queue *queue,
                                size_t global_connections, size_t global_limit,
                                bool stopping);
/* Move caller's valid owned FD on success, setting *client_fd=-1.
 * On full/invalid input caller retains its FD. No socket I/O or HTTP parsing. */
int enqueue_parent_client(struct parent_client_queue *queue, int *client_fd,
                           uint64_t connection_id, const struct timespec *accepted_at);
/* 1: moved front to output, 0: empty, -1: invalid/occupied output.
 * Caller initializes output.client_fd=-1; dequeue does not close it. */
int dequeue_parent_client(struct parent_client_queue *queue, struct parent_queued_client *client);
/* Close all still-queued FDs once, free storage, zero owner. Continue cleanup
 * after close error, return the first error. Does not close dequeued FDs. */
int destroy_parent_client_queue(struct parent_client_queue *queue);

#endif
