#ifndef PROCESS_SIGNAL_WAIT_H
#define PROCESS_SIGNAL_WAIT_H

#include <stdbool.h>
#include <signal.h>
#include <sys/epoll.h>

enum {
    PROCESS_SIGNAL_RUNNING = 1,
    PROCESS_SIGNAL_CHILD_PENDING = 2,
    PROCESS_SIGNAL_ACTION_COUNT = 4
};

/* One context per network/control thread. Helpers keep managed signals blocked. Caller owns a stable sig_atomic_t
 * (the existing process control flag) and a handler that calls record below.
 * No new mutable global is kept by this module. Fork inherits private copies. */
struct process_signal_wait {
    volatile sig_atomic_t *state;
    sigset_t managed_mask;
    sigset_t original_mask;
    sigset_t wait_mask;
    struct sigaction original_actions[PROCESS_SIGNAL_ACTION_COUNT];
    bool active;
};

/* Block INT/TERM/CHLD outside wait, install handler (all managed signals masked
 * inside it), ignore PIPE. Zero-initialize context. Initialize *state=RUNNING.
 * Keep managed signals blocked during all processing while context is active. */
int prepare_process_signal_wait(struct process_signal_wait *context,
                                volatile sig_atomic_t *state, void (*handler)(int));
/* Async handler helper: only sig_atomic_t accesses, no syscalls/errno changes.
 * Installed sa_mask serializes managed handlers; stop always dominates CHLD. */
void record_process_signal(volatile sig_atomic_t *state, int signal_number);
bool process_signal_shutdown_requested(const struct process_signal_wait *context);
/* Consume child marker while managed signals are blocked; does not waitpid. */
int take_process_child_signal(struct process_signal_wait *context);
/* Atomic unmask-and-wait, then briefly allow managed handlers and block again.
 * On successful mask changes, preserve wait return/errno. Mask errors return -1;
 * caller must stop normal I/O on such errors. No automatic EINTR retry: caller checks
 * state/fatal first, and may explicitly wait during shutdown to reap children.
 * n=0 timeout is polling; production deadlines/budgets are selected separately. */
int wait_process_signal_events(const struct process_signal_wait *context,
                               int epoll_fd, struct epoll_event *events,
                               int maximum_events, int timeout_ms);
/* Restore prior dispositions/mask after own children are reaped and signal
 * producers stopped. Pending signals may then follow the original policy. */
int restore_process_signal_wait(struct process_signal_wait *context);
/* Fork/exec child only: defaults for INT/TERM/CHLD/PIPE and pre-server mask.
 * Only async-signal-safe operations; caller execs or _exit on error. */
int restore_exec_child_signals(const struct process_signal_wait *context);

#endif
