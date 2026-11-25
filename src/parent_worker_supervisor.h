#ifndef PARENT_WORKER_SUPERVISOR_H
#define PARENT_WORKER_SUPERVISOR_H
#include "parent_client_queue.h"
#include "parent_connection_ledger.h"
#include "shared_backend_state.h"
#include "worker_fd_channel.h"
#include <sys/types.h>

enum parent_worker_phase { PARENT_WORKER_UNUSED, PARENT_WORKER_RUNNING,
                           PARENT_WORKER_STOPPING, PARENT_WORKER_REAPED,
                           PARENT_WORKER_FINISHED, PARENT_WORKER_LOST };
enum parent_worker_stop_reason { PARENT_STOP_CHANNEL = 1, PARENT_STOP_REQUEST,
    PARENT_STOP_SHARED_FATAL, PARENT_STOP_NO_WORKERS, PARENT_STOP_SYSTEM_ERROR };
enum parent_worker_retry_kind { PARENT_RETRY_NONE, PARENT_RETRY_WAIT, PARENT_RETRY_BACKEND };
enum parent_supervisor_failure { PARENT_SUPERVISOR_NONE, PARENT_SUPERVISOR_WAIT,
    PARENT_SUPERVISOR_SIGNAL, PARENT_SUPERVISOR_CLOSE, PARENT_SUPERVISOR_LEDGER,
    PARENT_SUPERVISOR_BACKEND };
struct parent_supervisor_diagnostic {
    uint32_t worker_index;
    uint64_t worker_generation;
    enum parent_supervisor_failure failed_operation;
    int system_error;
    struct backend_state_diagnostic backend;
};
struct parent_supervised_worker {
    pid_t pid;
    uint64_t generation, stop_deadline_ms, retry_at_ms;
    enum parent_worker_retry_kind retry_kind;
    int channel_fd, wait_status;
    enum parent_worker_phase phase;
    enum parent_worker_stop_reason stop_reason;
    bool term_attempted, kill_attempted, reaped, cleanup_failed;
    struct worker_fd_sender sender;
};
/* Parent-only. Owns worker table, adopted channels and senders. Borrows queue,
 * ledger and mapping until all children are reaped. Single waitpid owner;
 * no SIGCHLD SIG_IGN/SA_NOCLDWAIT, threads, respawn, I/O under shared lock. */
struct parent_worker_supervisor {
    struct parent_supervised_worker *workers;
    size_t capacity;
    struct parent_client_queue *queue;
    struct parent_connection_ledger *ledger;
    struct shared_backend_memory *backend;
    uint64_t last_now_ms;
    bool started, stopping, exit_error, cleanup_pending;
    struct parent_supervisor_diagnostic diagnostic;
};
int read_parent_supervisor_time_ms(uint64_t *now_ms);
int create_parent_worker_supervisor(struct parent_connection_ledger *ledger,
    struct shared_backend_memory *backend, struct parent_client_queue *queue,
    struct parent_worker_supervisor *supervisor);
/* Called in parent after fork and both shared/parent generations activated.
 * Success moves *channel_fd to supervisor (-1). No process creation here. */
int adopt_parent_worker(struct parent_worker_supervisor *supervisor, uint32_t worker,
                         uint64_t generation, pid_t pid, int *channel_fd);
int finish_parent_worker_startup(struct parent_worker_supervisor *supervisor);
bool parent_worker_accepts_assignment(const struct parent_worker_supervisor *supervisor,
                                       uint32_t worker);
bool parent_supervisor_accepts_clients(const struct parent_worker_supervisor *supervisor);
/* Requests only: no signals until advance, repeated requests never extend grace. */
int request_parent_worker_stop(struct parent_worker_supervisor *supervisor,
    uint32_t worker, uint64_t now_ms, enum parent_worker_stop_reason reason);
int request_parent_supervisor_shutdown(struct parent_worker_supervisor *supervisor,
    uint64_t now_ms, enum parent_worker_stop_reason reason, bool error_exit);
/* One WNOHANG wait per owned PID and at most one shared trylock per reaped worker.
 * Deferred WAIT/BACKEND work runs only when its per-worker retry is due.
 * 0 no deferred cleanup, 1 mutex busy/EINTR pending, -1 invalid caller input.
 * Runtime failures set bounded first diagnostic/exit_error and request shutdown;
 * caller must keep advancing to reap children. No logging/sleep/spin here. */
int advance_parent_worker_supervisor(struct parent_worker_supervisor *supervisor, uint64_t now_ms);
/* Signal-only compatibility helper, -1 when no signal deadline exists. */
int parent_supervisor_signal_timeout_ms(const struct parent_worker_supervisor *supervisor, uint64_t now_ms);
/* Parent loop uses min(signal deadline, pending control retry). -1 means no
 * deadline; include other HTTP/dispatch deadlines separately at integration. */
int parent_supervisor_wait_timeout_ms(const struct parent_worker_supervisor *supervisor, uint64_t now_ms);
bool parent_supervisor_all_reaped(const struct parent_worker_supervisor *supervisor);
bool parent_supervisor_cleanup_finished(const struct parent_worker_supervisor *supervisor);
/* EBUSY if any adopted PID is unconfirmed or cleanup is pending. Does not destroy
 * borrowed owners or mapping. Unregistered slots are harmless startup failures. */
int destroy_parent_worker_supervisor(struct parent_worker_supervisor *supervisor);
#endif
