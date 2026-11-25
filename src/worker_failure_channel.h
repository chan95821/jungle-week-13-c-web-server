#ifndef WORKER_FAILURE_CHANNEL_H
#define WORKER_FAILURE_CHANNEL_H

#include "shared_backend_state.h"

enum worker_failure_snapshot {
    WORKER_FAILURE_NO_REPORT, WORKER_FAILURE_REPORT_READY,
    WORKER_FAILURE_REPORT_INCOMPLETE, WORKER_FAILURE_REPORT_STALE
};
enum worker_failure_notify {
    WORKER_FAILURE_NOTIFIED, WORKER_FAILURE_ALREADY_READABLE, WORKER_FAILURE_NOTIFY_RETRY
};
enum worker_failure_wakeup {
    WORKER_FAILURE_NO_WAKEUP, WORKER_FAILURE_WAKEUP_RECEIVED,
    WORKER_FAILURE_WAKEUP_EOF, WORKER_FAILURE_READ_RETRY
};
enum { WORKER_FAILURE_LOG_BYTES = 384 };

/* POSIX pipe+fcntl, initialized before fork. Caller owns the two FDs and closes
 * inherited unused ends. SIGPIPE must already be ignored (server policy). */
int create_worker_failure_pipe(int wakeup[2]);
/* Parent only before initial fork / after waitpid confirmed old writer death.
 * Monotonic generation; refuse re-arm after a global fatal (retain evidence). */
int arm_worker_failure_slot(struct shared_backend_memory *memory, uint32_t worker,
                             uint64_t generation);
/* Outside backend lock, one writer per armed slot. 0 published, 1 existing
 * first report retained, -1 error. Publication itself makes fatal sticky.
 * Pipe notification is separate; death between the two needs SIGCHLD handling. */
int publish_worker_failure(struct shared_backend_memory *memory, uint32_t worker,
                           uint64_t generation, const struct backend_state_diagnostic *diagnostic);
/* Single non-blocking write/read. EAGAIN on write => already readable; EINTR
 * asks caller to retry later. EPIPE is an error, not a reason to clear fatal. */
int notify_parent_worker_failure(int write_fd);
int consume_worker_failure_wakeup(int read_fd);
/* Single parent reader. No payload access for EMPTY/WRITING/stale generation.
 * Parent must not re-arm concurrently with a read. -1 => untrusted report. */
int read_worker_failure_report(struct shared_backend_memory *memory, uint32_t worker,
                               uint64_t generation, struct worker_failure_report *report);
/* Fixed numeric fields only; no sink I/O here, no arbitrary shared strings. */
int format_worker_failure_report(char *buffer, size_t capacity,
                                 const struct worker_failure_report *report);
/* Missing/incomplete/stale/untrusted snapshot: trusted parent identity only.
 * Never inspect unpublished payload or invent its failed check/stage. */
int format_missing_worker_failure_report(char *buffer, size_t capacity,
                                         uint32_t worker, uint64_t generation, int snapshot);

#ifdef WORKER_FAILURE_CHANNEL_TEST
enum worker_failure_fault_step {
    WORKER_FAILURE_INTENT_MARKED = 1, WORKER_FAILURE_CLAIMED, WORKER_FAILURE_PAYLOAD_WRITTEN,
    WORKER_FAILURE_PUBLISHED, WORKER_FAILURE_PIPE_WRITTEN
};
void worker_failure_fault_step(enum worker_failure_fault_step step);
#endif

#endif
