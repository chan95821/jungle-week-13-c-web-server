#define _GNU_SOURCE
#include "process_signal_wait.h"

#include <errno.h>
#include <stddef.h>
#include <pthread.h>
/* The network thread owns signal delivery; helper threads inherit managed
 * signals blocked. pthread_sigmask is defined for multithreaded workers. */
static int change_thread_signal_mask(int how,const sigset_t *set,sigset_t *prior){
    int error=pthread_sigmask(how,set,prior);if(error){errno=error;return -1;}return 0;
}

/* Read-only signal identifiers, not shared connection/control state. */
static const int owned_signals[PROCESS_SIGNAL_ACTION_COUNT] = {SIGINT, SIGTERM, SIGCHLD, SIGPIPE};

#ifdef PROCESS_SIGNAL_WAIT_TEST
extern int process_test_epoll_pwait(int fd, struct epoll_event *events, int count,
                                    int timeout, const sigset_t *mask);
#define signal_epoll_pwait process_test_epoll_pwait
#else
#define signal_epoll_pwait epoll_pwait
#endif

int prepare_process_signal_wait(struct process_signal_wait *context,
                                volatile sig_atomic_t *state, void (*handler)(int)) {
    if (!context || !state || !handler) { errno = EINVAL; return -1; }
    if (context->active) { errno = EBUSY; return -1; }
    struct process_signal_wait prepared = {.state = state};
    if (sigemptyset(&prepared.managed_mask) < 0) return -1;
    for (size_t i = 0; i < 3; ++i)
        if (sigaddset(&prepared.managed_mask, owned_signals[i]) < 0) return -1;
    if (change_thread_signal_mask(SIG_BLOCK, &prepared.managed_mask, &prepared.original_mask) < 0) return -1;
    size_t installed = 0;
    for (size_t i = 0; i < PROCESS_SIGNAL_ACTION_COUNT; ++i)
        if (sigaction(owned_signals[i], NULL, &prepared.original_actions[i]) < 0) goto error;
    prepared.wait_mask = prepared.original_mask;
    for (size_t i = 0; i < 3; ++i)
        if (sigdelset(&prepared.wait_mask, owned_signals[i]) < 0) goto error;
    for (size_t i = 0; i < PROCESS_SIGNAL_ACTION_COUNT; ++i) {
        struct sigaction action = {0};
        action.sa_mask = prepared.managed_mask;
        action.sa_handler = owned_signals[i] == SIGPIPE ? SIG_IGN : handler;
        if (sigaction(owned_signals[i], &action, NULL) < 0) goto error;
        ++installed;
    }
    *state = PROCESS_SIGNAL_RUNNING;
    prepared.active = true;
    *context = prepared;
    return 0;
error:
    {
        int saved_errno = errno;
        while (installed) {
            --installed;
            sigaction(owned_signals[installed], &prepared.original_actions[installed], NULL);
        }
        change_thread_signal_mask(SIG_SETMASK, &prepared.original_mask, NULL);
        errno = saved_errno;
        return -1;
    }
}

void record_process_signal(volatile sig_atomic_t *state, int signal_number) {
    if (!state) return;
    if (signal_number == SIGINT || signal_number == SIGTERM) *state = 0;
    else if (signal_number == SIGCHLD && (*state & PROCESS_SIGNAL_RUNNING))
        *state = *state | PROCESS_SIGNAL_CHILD_PENDING;
}

bool process_signal_shutdown_requested(const struct process_signal_wait *context) {
    return !context || !context->active || !context->state ||
           !(*context->state & PROCESS_SIGNAL_RUNNING);
}

int take_process_child_signal(struct process_signal_wait *context) {
    if (!context || !context->active || !context->state) { errno = EINVAL; return -1; }
    sig_atomic_t state = *context->state;
    *context->state = state & PROCESS_SIGNAL_RUNNING;
    return (state & PROCESS_SIGNAL_CHILD_PENDING) != 0;
}

int wait_process_signal_events(const struct process_signal_wait *context,
                               int epoll_fd, struct epoll_event *events,
                               int maximum_events, int timeout_ms) {
    if (!context || !context->active || !context->state || !events ||
        maximum_events <= 0 || timeout_ms < -1) { errno = EINVAL; return -1; }
    int result = signal_epoll_pwait(epoll_fd, events, maximum_events, timeout_ms, &context->wait_mask);
    int wait_errno = errno;
    /* Ready LT I/O may return before pending signals are delivered. Give the
     * minimal handlers a delivery window before the caller examines flags. */
    int mask_error = 0;
    if (change_thread_signal_mask(SIG_SETMASK, &context->wait_mask, NULL) < 0) mask_error = errno;
    if (change_thread_signal_mask(SIG_BLOCK, &context->managed_mask, NULL) < 0 && !mask_error)
        mask_error = errno;
    if (mask_error) { errno = mask_error; return -1; }
    errno = wait_errno;
    return result;
}

int restore_process_signal_wait(struct process_signal_wait *context) {
    if (!context) { errno = EINVAL; return -1; }
    if (!context->active) return 0;
    int first_error = 0;
    for (size_t i = PROCESS_SIGNAL_ACTION_COUNT; i > 0; --i)
        if (sigaction(owned_signals[i - 1], &context->original_actions[i - 1], NULL) < 0 && !first_error)
            first_error = errno;
    if (change_thread_signal_mask(SIG_SETMASK, &context->original_mask, NULL) < 0 && !first_error)
        first_error = errno;
    if (first_error) { errno = first_error; return -1; }
    context->active = false;
    context->state = NULL;
    return 0;
}

int restore_exec_child_signals(const struct process_signal_wait *context) {
    if (!context || !context->active) { errno = EINVAL; return -1; }
    struct sigaction action = {.sa_handler = SIG_DFL};
    if (sigemptyset(&action.sa_mask) < 0) return -1;
    for (size_t i = 0; i < PROCESS_SIGNAL_ACTION_COUNT; ++i)
        if (sigaction(owned_signals[i], &action, NULL) < 0) return -1;
    return sigprocmask(SIG_SETMASK, &context->original_mask, NULL);
}
