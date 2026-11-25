#ifndef WORKER_CLOSED_CHANNEL_H
#define WORKER_CLOSED_CHANNEL_H
#include "distribution_policy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { WORKER_CLOSED_FRAME_BYTES = 56 };
enum worker_closed_progress { WORKER_CLOSED_PENDING, WORKER_CLOSED_READY, WORKER_CLOSED_EOF };
struct worker_connection_closed {
    uint32_t worker_index;
    uint64_t worker_generation;
    uint64_t connection_id;
    struct distribution_observation observation;
};
/* Worker-only bounded FIFO. Count includes the partially sent head.
 * One queue/writer per channel. No FD ownership, ACK, heap growth or retry loop.
 * Caller ignores SIGPIPE process-wide as established by server policy. */
struct worker_closed_queue {
    struct worker_connection_closed *items;
    size_t capacity, head, count, sent_bytes;
    unsigned char frame[WORKER_CLOSED_FRAME_BYTES];
    bool failed;
};
struct worker_closed_receiver {
    unsigned char frame[WORKER_CLOSED_FRAME_BYTES];
    size_t received_bytes;
    bool failed;
};
int create_worker_closed_queue(size_t capacity, struct worker_closed_queue *queue);
void destroy_worker_closed_queue(struct worker_closed_queue *queue);
/* Capacity gate for NEW FD-frame reception, never existing client/control I/O.
 * Worker must additionally enforce its assigned connection/admission limits. */
bool worker_closed_has_capacity(const struct worker_closed_queue *queue);
/* 0 queued, 1 duplicate already queued, -1 invalid/full. No socket/FD access. */
int enqueue_worker_connection_closed(struct worker_closed_queue *queue,
                                      const struct worker_connection_closed *closed);
/* At most one syscall per call; EAGAIN/EINTR pending. READY pops exactly one.
 * EOF/invalid control is an error, channel must be retired (no resync). */
int advance_worker_closed_send(int channel_fd, struct worker_closed_queue *queue);
/* READY publishes a validated complete frame; output unchanged otherwise.
 * Unexpected SCM_RIGHTS are closed and rejected, never exposed to caller. */
int advance_worker_closed_receive(int channel_fd, uint32_t expected_worker,
                                   uint64_t expected_generation,
                                   struct worker_closed_receiver *receiver,
                                   struct worker_connection_closed *closed);
#endif
