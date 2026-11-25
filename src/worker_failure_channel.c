#define _POSIX_C_SOURCE 200809L
#include "worker_failure_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

#ifdef WORKER_FAILURE_CHANNEL_TEST
extern ssize_t worker_failure_test_write(int fd, const void *buffer, size_t bytes);
extern ssize_t worker_failure_test_read(int fd, void *buffer, size_t bytes);
#define failure_write worker_failure_test_write
#define failure_read worker_failure_test_read
#define CHECKPOINT(step) worker_failure_fault_step(step)
#else
#define failure_write write
#define failure_read read
#define CHECKPOINT(step) ((void)0)
#endif

static int valid_slot(const struct shared_backend_memory *memory, uint32_t worker);
static int reject_report(struct shared_backend_memory *memory);

int create_worker_failure_pipe(int wakeup[2]) {
    if (!wakeup) { errno = EINVAL; return -1; }
    wakeup[0] = wakeup[1] = -1;
    if (pipe(wakeup) < 0) return -1;
    for (size_t i = 0; i < 2; ++i) {
        int flags = fcntl(wakeup[i], F_GETFL);
        if (flags < 0 || fcntl(wakeup[i], F_SETFL, flags | O_NONBLOCK) < 0) goto error;
        flags = fcntl(wakeup[i], F_GETFD);
        if (flags < 0 || fcntl(wakeup[i], F_SETFD, flags | FD_CLOEXEC) < 0) goto error;
    }
    return 0;
error:
    {
        int saved_errno = errno;
        close(wakeup[0]); close(wakeup[1]);
        wakeup[0] = wakeup[1] = -1;
        errno = saved_errno;
        return -1;
    }
}

int arm_worker_failure_slot(struct shared_backend_memory *memory, uint32_t worker,
                             uint64_t generation) {
    if (!valid_slot(memory, worker) || !generation) { errno = EINVAL; return -1; }
    if (atomic_load_explicit(&memory->header->fatal, memory_order_seq_cst)) { errno = EUCLEAN; return -1; }
    struct shared_worker_failure *slot = &memory->worker_failures[worker];
    if (slot->reserved || atomic_load_explicit(&slot->publication, memory_order_acquire) != WORKER_FAILURE_EMPTY)
        return reject_report(memory);
    if (generation <= atomic_load_explicit(&slot->generation, memory_order_acquire)) {
        errno = ESTALE;
        return -1;
    }
    /* No old writer may still exist here; parent owns this lifecycle boundary. */
    slot->report = (struct worker_failure_report){0};
    atomic_store_explicit(&slot->generation, generation, memory_order_release);
    atomic_store_explicit(&slot->publication, WORKER_FAILURE_EMPTY, memory_order_release);
    return 0;
}

int publish_worker_failure(struct shared_backend_memory *memory, uint32_t worker,
                           uint64_t generation, const struct backend_state_diagnostic *diagnostic) {
    if (!valid_slot(memory, worker) || !generation || !diagnostic) { errno = EINVAL; return -1; }
    if (memory->lock_held) { errno = EPERM; return -1; }
    struct shared_worker_failure *slot = &memory->worker_failures[worker];
    if (generation != atomic_load_explicit(&slot->generation, memory_order_acquire)) {
        errno = ESTALE;
        return -1;
    }
    if (slot->reserved) return reject_report(memory);
    /* Mark intent first: a death before claiming/publishing must not hide fatal. */
    atomic_store_explicit(&memory->header->fatal, 1, memory_order_seq_cst);
    CHECKPOINT(WORKER_FAILURE_INTENT_MARKED);
    unsigned int expected = WORKER_FAILURE_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(&slot->publication, &expected, WORKER_FAILURE_WRITING,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        if (expected == WORKER_FAILURE_READY) return 1;
        if (expected == WORKER_FAILURE_WRITING) { errno = EBUSY; return -1; }
        return reject_report(memory);
    }
    CHECKPOINT(WORKER_FAILURE_CLAIMED);
    slot->report = (struct worker_failure_report){
        .reporter_index = worker, .reason = WORKER_FAILURE_SHARED_STATE,
        .reporter_generation = generation, .diagnostic = *diagnostic
    };
    CHECKPOINT(WORKER_FAILURE_PAYLOAD_WRITTEN);
    atomic_store_explicit(&slot->publication, WORKER_FAILURE_READY, memory_order_release);
    CHECKPOINT(WORKER_FAILURE_PUBLISHED);
    return 0;
}

int notify_parent_worker_failure(int write_fd) {
    const unsigned char notification = 1;
    ssize_t count = failure_write(write_fd, &notification, 1);
    if (count == 1) {
        CHECKPOINT(WORKER_FAILURE_PIPE_WRITTEN);
        return WORKER_FAILURE_NOTIFIED;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return WORKER_FAILURE_ALREADY_READABLE;
    if (count < 0 && errno == EINTR) return WORKER_FAILURE_NOTIFY_RETRY;
    if (count >= 0) errno = EIO;
    return -1;
}

int consume_worker_failure_wakeup(int read_fd) {
    unsigned char notifications[64];
    ssize_t count = failure_read(read_fd, notifications, sizeof(notifications));
    if (count > 0) return WORKER_FAILURE_WAKEUP_RECEIVED;
    if (count == 0) return WORKER_FAILURE_WAKEUP_EOF;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return WORKER_FAILURE_NO_WAKEUP;
    if (errno == EINTR) return WORKER_FAILURE_READ_RETRY;
    return -1;
}

int read_worker_failure_report(struct shared_backend_memory *memory, uint32_t worker,
                               uint64_t generation, struct worker_failure_report *report) {
    if (!valid_slot(memory, worker) || !generation || !report) { errno = EINVAL; return -1; }
    struct shared_worker_failure *slot = &memory->worker_failures[worker];
    if (generation != atomic_load_explicit(&slot->generation, memory_order_acquire))
        return WORKER_FAILURE_REPORT_STALE;
    unsigned int state = atomic_load_explicit(&slot->publication, memory_order_acquire);
    if (slot->reserved) return reject_report(memory);
    if (state == WORKER_FAILURE_EMPTY) return WORKER_FAILURE_NO_REPORT;
    if (state == WORKER_FAILURE_WRITING) return WORKER_FAILURE_REPORT_INCOMPLETE;
    if (state != WORKER_FAILURE_READY || slot->reserved) return reject_report(memory);
    /* Writer never changes a published payload; no re-arm during this read. */
    struct worker_failure_report snapshot = slot->report;
    if (snapshot.reporter_index != worker || snapshot.reporter_generation != generation ||
        snapshot.reason != WORKER_FAILURE_SHARED_STATE) return reject_report(memory);
    *report = snapshot;
    return WORKER_FAILURE_REPORT_READY;
}

int format_worker_failure_report(char *buffer, size_t capacity,
                                 const struct worker_failure_report *report) {
    if (!buffer || !capacity || !report) { errno = EINVAL; return -1; }
    int count = snprintf(buffer, capacity,
        "worker-failure reporter=%" PRIu32 " generation=%" PRIu64
        " affected_worker=%" PRIu32 " affected_generation=%" PRIu64
        " failed_check=%u recovery_phase=%u recovery_stage=%u reason=%" PRIu32 "\n",
        report->reporter_index, report->reporter_generation,
        report->diagnostic.worker_index, report->diagnostic.worker_generation,
        (unsigned int)report->diagnostic.failed_check, report->diagnostic.recovery_phase,
        report->diagnostic.recovery_stage, report->reason);
    if (count < 0 || (size_t)count >= capacity) { errno = ENOSPC; return -1; }
    return count;
}

int format_missing_worker_failure_report(char *buffer, size_t capacity,
                                         uint32_t worker, uint64_t generation, int snapshot) {
    if (!buffer || !capacity || !generation) { errno = EINVAL; return -1; }
    const char *reason;
    switch (snapshot) {
        case WORKER_FAILURE_NO_REPORT: reason = "not_published"; break;
        case WORKER_FAILURE_REPORT_INCOMPLETE: reason = "incomplete_publication"; break;
        case WORKER_FAILURE_REPORT_STALE: reason = "stale_generation"; break;
        case -1: reason = "untrusted_report"; break;
        default: errno = EINVAL; return -1;
    }
    int count = snprintf(buffer, capacity,
        "worker-failure reporter=%" PRIu32 " generation=%" PRIu64
        " affected_worker=unknown affected_generation=unknown failed_check=unknown"
        " recovery_phase=unknown recovery_stage=unknown reason=%s\n",
        worker, generation, reason);
    if (count < 0 || (size_t)count >= capacity) { errno = ENOSPC; return -1; }
    return count;
}
static int valid_slot(const struct shared_backend_memory *memory, uint32_t worker) {
    /* Use trusted process-local bounds, not potentially damaged shared header. */
    return memory && memory->header && memory->worker_failures && worker < memory->layout.worker_capacity;
}

static int reject_report(struct shared_backend_memory *memory) {
    atomic_store_explicit(&memory->header->fatal, 1, memory_order_seq_cst);
    errno = EUCLEAN;
    return -1;
}
