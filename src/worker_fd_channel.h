#ifndef WORKER_FD_CHANNEL_H
#define WORKER_FD_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { WORKER_FD_FRAME_BYTES = 48 };
enum worker_fd_progress { WORKER_FD_PENDING, WORKER_FD_READY, WORKER_FD_EOF, WORKER_FD_CLIENT_GONE };

struct worker_client_metadata {
    uint32_t worker_index;
    uint64_t worker_generation;
    uint64_t connection_id;
    struct timespec accepted_at;
};

/* Zero-initialize. One sender per channel; never interleave outgoing frames.
 * prepare moves ownership from *client_fd, setting it to -1 on success. */
struct worker_fd_sender {
    unsigned char frame[WORKER_FD_FRAME_BYTES];
    size_t sent_bytes;
    int client_fd;
    bool owns_client_fd;
    bool fd_sent;
    bool finished;
    bool failed;
};

/* Zero-initialize. Held descriptor is private until the whole frame validates. */
struct worker_fd_receiver {
    unsigned char frame[WORKER_FD_FRAME_BYTES];
    size_t received_bytes;
    int client_fd;
    bool owns_client_fd;
    bool failed;
};

struct worker_received_client {
    int client_fd; /* Caller initializes to -1 and owns only after READY. */
    struct worker_client_metadata metadata;
};

int create_worker_fd_channel(int channel[2]);
int prepare_worker_fd_transfer(struct worker_fd_sender *sender, int *client_fd,
                               const struct worker_client_metadata *metadata);
/* At most one non-blocking sendmsg/recvmsg per call. EINTR/EAGAIN => PENDING.
 * READY closes sender's original after all bytes; receiver transfers FD out.
 * CLIENT_GONE: validated whole frame, accepted STREAM reset before delivery;
 * receiver closes FD, outputs metadata/client_fd=-1 for normal CLOSED accounting.
 * On fatal error close the channel; no resync/retry on another worker after
 * fd_sent. Abort is idempotent and closes only still-owned descriptors. */
int advance_worker_fd_send(int channel_fd, struct worker_fd_sender *sender);
int advance_worker_fd_receive(int channel_fd, uint32_t expected_worker,
                              uint64_t expected_generation,
                              struct worker_fd_receiver *receiver,
                              struct worker_received_client *received);
void abort_worker_fd_send(struct worker_fd_sender *sender);
void abort_worker_fd_receive(struct worker_fd_receiver *receiver);

#endif
