#define _GNU_SOURCE
#include "../../src/process_signal_wait.h"
#include "../../src/worker_failure_channel.h"
#include "../../src/parent_client_queue.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t signal_state;
static int boundary_signal;
static int wait_success_errno;

static void on_signal(int signal_number) { record_process_signal(&signal_state, signal_number); }

int process_test_epoll_pwait(int fd, struct epoll_event *events, int count,
                            int timeout, const sigset_t *mask) {
    if (boundary_signal) {
        int signal_number = boundary_signal; boundary_signal = 0;
        sigset_t current;
        assert(sigprocmask(SIG_SETMASK, NULL, &current) == 0 && sigismember(&current, signal_number) == 1);
        sig_atomic_t previous = signal_state;
        assert(kill(getpid(), signal_number) == 0);
        assert(signal_state == previous); /* Handler cannot run before atomic wait. */
    }
    int result = epoll_pwait(fd, events, count, timeout, mask);
    if (result >= 0 && wait_success_errno) errno = wait_success_errno;
    return result;
}

static size_t open_fd_count(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    size_t count = 0; struct dirent *entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++count;
    assert(closedir(directory) == 0);
    return count;
}

static void assert_managed_blocked(void) {
    sigset_t current; assert(sigprocmask(SIG_SETMASK, NULL, &current) == 0);
    assert(sigismember(&current, SIGINT) == 1 && sigismember(&current, SIGTERM) == 1 &&
           sigismember(&current, SIGCHLD) == 1);
}

static bool managed_pending(void) {
    sigset_t pending; assert(sigpending(&pending) == 0);
    return sigismember(&pending, SIGINT) == 1 || sigismember(&pending, SIGTERM) == 1 ||
           sigismember(&pending, SIGCHLD) == 1;
}

static void drain_test_signals(struct process_signal_wait *context, int epoll_fd) {
    for (size_t i = 0; i < 16 && managed_pending(); ++i) {
        struct epoll_event event;
        int result = wait_process_signal_events(context, epoll_fd, &event, 1, 0);
        assert(result >= 0 || errno == EINTR);
    }
    assert(!managed_pending());
}

static void prepare_fixture(struct process_signal_wait *context, int *epoll_fd) {
    *context = (struct process_signal_wait){0}; signal_state = 0;
    assert(prepare_process_signal_wait(context, &signal_state, on_signal) == 0);
    assert(signal_state == PROCESS_SIGNAL_RUNNING);
    assert_managed_blocked();
    *epoll_fd = epoll_create1(EPOLL_CLOEXEC); assert(*epoll_fd >= 0);
}

static void finish_fixture(struct process_signal_wait *context, int epoll_fd) {
    drain_test_signals(context, epoll_fd);
    close(epoll_fd);
    assert(restore_process_signal_wait(context) == 0);
    assert(restore_process_signal_wait(context) == 0);
}

static void test_pending_and_boundary_signals(void) {
    const int signals[] = {SIGINT, SIGTERM, SIGCHLD};
    for (unsigned int boundary = 0; boundary < 2; ++boundary) {
        for (size_t i = 0; i < 3; ++i) {
            struct process_signal_wait context; int epoll_fd;
            prepare_fixture(&context, &epoll_fd);
            assert(!process_signal_shutdown_requested(&context));
            if (boundary) boundary_signal = signals[i];
            else {
                assert(kill(getpid(), signals[i]) == 0);
                assert(signal_state == PROCESS_SIGNAL_RUNNING && managed_pending());
            }
            struct epoll_event event;
            assert(wait_process_signal_events(&context, epoll_fd, &event, 1, 1000) == -1 && errno == EINTR);
            assert_managed_blocked();
            if (signals[i] == SIGCHLD) {
                assert(take_process_child_signal(&context) == 1 && !process_signal_shutdown_requested(&context));
                assert(take_process_child_signal(&context) == 0);
            } else {
                assert(process_signal_shutdown_requested(&context));
                record_process_signal(&signal_state, SIGCHLD);
                assert(signal_state == 0 && take_process_child_signal(&context) == 0);
            }
            finish_fixture(&context, epoll_fd);
        }
    }
}

static void test_stop_priority_and_ready_io(void) {
    struct process_signal_wait context; int epoll_fd;
    prepare_fixture(&context, &epoll_fd);
    record_process_signal(&signal_state, SIGCHLD);
    assert(signal_state == (PROCESS_SIGNAL_RUNNING | PROCESS_SIGNAL_CHILD_PENDING));
    record_process_signal(&signal_state, SIGTERM);
    record_process_signal(&signal_state, SIGCHLD);
    assert(signal_state == 0 && take_process_child_signal(&context) == 0);
    finish_fixture(&context, epoll_fd);

    prepare_fixture(&context, &epoll_fd);
    int ready[2]; assert(pipe(ready) == 0);
    struct epoll_event registration = {.events = EPOLLIN, .data.u64 = 77};
    assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ready[0], &registration) == 0);
    assert(write(ready[1], "R", 1) == 1);
    assert(kill(getpid(), SIGCHLD) == 0 && kill(getpid(), SIGTERM) == 0);
    /* Keep I/O readable: shutdown must not be indefinitely starved by LT data. */
    for (size_t i = 0; i < 16 && !process_signal_shutdown_requested(&context); ++i) {
        struct epoll_event event;
        int result = wait_process_signal_events(&context, epoll_fd, &event, 1, 1000);
        assert(result == 1 || (result == -1 && errno == EINTR));
    }
    assert(process_signal_shutdown_requested(&context));
    close(ready[0]); close(ready[1]);
    finish_fixture(&context, epoll_fd);
}

static void test_wait_return_contract(void) {
    struct process_signal_wait context; int epoll_fd;
    prepare_fixture(&context, &epoll_fd);
    struct epoll_event event;
    assert(wait_process_signal_events(&context, -1, &event, 1, 0) == -1 && errno == EBADF);
    assert_managed_blocked();
    wait_success_errno = ERANGE; /* Wrapper stamps errno after the real syscall. */
    assert(wait_process_signal_events(&context, epoll_fd, &event, 1, 0) == 0 && errno == ERANGE);
    assert_managed_blocked();
    int ready[2]; assert(pipe(ready) == 0);
    struct epoll_event registration = {.events = EPOLLIN, .data.u64 = 91};
    assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ready[0], &registration) == 0);
    assert(write(ready[1], "R", 1) == 1);
    assert(kill(getpid(), SIGTERM) == 0);
    int result = wait_process_signal_events(&context, epoll_fd, &event, 1, 0);
    assert(result == 1 || (result == -1 && errno == EINTR));
    if (result == 1) assert(event.data.u64 == 91 && errno == ERANGE);
    assert(process_signal_shutdown_requested(&context));
    assert_managed_blocked();
    /* With no pending signals, positive readiness and stamped errno survive. */
    assert(wait_process_signal_events(&context, epoll_fd, &event, 1, 0) == 1);
    assert(event.data.u64 == 91 && errno == ERANGE);
    assert_managed_blocked();
    wait_success_errno = 0;
    close(ready[0]); close(ready[1]);
    finish_fixture(&context, epoll_fd);
}

static void test_async_shutdown_from_child(void) {
    const int signals[] = {SIGINT, SIGTERM};
    for (size_t i = 0; i < 2; ++i) {
        struct process_signal_wait context; int epoll_fd;
        prepare_fixture(&context, &epoll_fd);
        int start[2]; assert(pipe(start) == 0);
        pid_t parent = getpid(), child = fork(); assert(child >= 0);
        if (child == 0) {
            close(start[1]); close(epoll_fd);
            char marker; if (read(start[0], &marker, 1) != 1) _exit(2);
            if (kill(parent, signals[i]) < 0) _exit(3);
            _exit(0);
        }
        close(start[0]); assert(write(start[1], "R", 1) == 1); close(start[1]);
        for (size_t turn = 0; turn < 8 && !process_signal_shutdown_requested(&context); ++turn) {
            struct epoll_event event;
            int result = wait_process_signal_events(&context, epoll_fd, &event, 1, 1000);
            assert(result >= 0 || errno == EINTR);
        }
        assert(process_signal_shutdown_requested(&context));
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        finish_fixture(&context, epoll_fd);
    }
}

static void test_multiple_children_caller_reaps(void) {
    struct process_signal_wait context; int epoll_fd;
    prepare_fixture(&context, &epoll_fd);
    int start[2]; assert(pipe(start) == 0);
    pid_t children[3]; bool reaped[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        children[i] = fork(); assert(children[i] >= 0);
        if (children[i] == 0) {
            close(start[1]); close(epoll_fd);
            char marker; if (read(start[0], &marker, 1) != 1) _exit(2);
            _exit(20 + (int)i);
        }
    }
    close(start[0]); assert(write(start[1], "RRR", 3) == 3); close(start[1]);
    size_t count = 0;
    for (size_t turn = 0; turn < 12 && count < 3; ++turn) {
        struct epoll_event event;
        int result = wait_process_signal_events(&context, epoll_fd, &event, 1, 1000);
        assert(result >= 0 || errno == EINTR);
        assert(take_process_child_signal(&context) >= 0);
        for (size_t i = 0; i < 3; ++i) if (!reaped[i]) {
            int status; pid_t found = waitpid(children[i], &status, WNOHANG);
            assert(found == 0 || found == children[i]); /* Library did not consume the status. */
            if (found > 0) {
                assert(WIFEXITED(status) && WEXITSTATUS(status) == 20 + (int)i);
                reaped[i] = true; ++count;
            }
        }
    }
    assert(count == 3);
    finish_fixture(&context, epoll_fd);
}

static void test_fatal_before_pipe_wakeup(void) {
    for (unsigned int incomplete = 0; incomplete < 2; ++incomplete) {
        struct process_signal_wait context; int epoll_fd;
        prepare_fixture(&context, &epoll_fd);
        struct shared_backend_memory memory = {0};
        assert(create_shared_backend_memory(1, 2, 4096, &memory) == 0);
        assert(arm_worker_failure_slot(&memory, 0, 1) == 0);
        int wakeup[2]; assert(create_worker_failure_pipe(wakeup) == 0);
        struct epoll_event registration = {.events = EPOLLIN, .data.u64 = 31};
        assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wakeup[0], &registration) == 0);
        struct parent_client_queue queue = {0}; assert(create_parent_client_queue(1, &queue) == 0);
        assert(parent_accept_has_capacity(&queue, 0, 1, false));
        pid_t child = fork(); assert(child >= 0);
        if (child == 0) {
            close(epoll_fd); close(wakeup[0]);
            if (incomplete) {
                /* Model a writer dying in publication; actual internal SIGKILL
                 * checkpoints were covered by test_worker_failure_channel. */
                atomic_store(&memory.header->fatal, 1);
                atomic_store(&memory.worker_failures[0].publication, WORKER_FAILURE_WRITING);
            } else {
                struct backend_state_diagnostic diagnostic = {
                    .failed_check = BACKEND_CHECK_JOURNAL, .recovery_stage = BACKEND_STAGE_JOURNAL
                };
                if (publish_worker_failure(&memory, 0, 1, &diagnostic) != 0) _exit(2);
            }
            raise(SIGKILL); /* Never write the wakeup pipe. */
            _exit(90);
        }
        /* Parent retains writer to model other live workers, so no EOF wake. */
        struct epoll_event event;
        assert(wait_process_signal_events(&context, epoll_fd, &event, 1, 1000) == -1 && errno == EINTR);
        assert(take_process_child_signal(&context) == 1);
        int status; assert(waitpid(child, &status, WNOHANG) == child && WIFSIGNALED(status));
        assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_NO_WAKEUP);
        assert(atomic_load(&memory.header->fatal) == 1);
        struct worker_failure_report report;
        int snapshot = read_worker_failure_report(&memory, 0, 1, &report);
        assert(snapshot == (incomplete ? WORKER_FAILURE_REPORT_INCOMPLETE : WORKER_FAILURE_REPORT_READY));
        assert(!parent_accept_has_capacity(&queue, 0, 1, atomic_load(&memory.header->fatal) != 0));
        if (incomplete) {
            char line[WORKER_FAILURE_LOG_BYTES];
            assert(format_missing_worker_failure_report(line, sizeof(line), 0, 1, snapshot) > 0);
            assert(strstr(line, "failed_check=unknown"));
        }
        close(wakeup[0]); close(wakeup[1]);
        assert(destroy_parent_client_queue(&queue) == 0);
        assert(destroy_shared_backend_memory(&memory) == 0);
        finish_fixture(&context, epoll_fd);
    }
}

static int exec_probe(void) {
    sigset_t mask; assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(sigismember(&mask, SIGINT) == 0 && sigismember(&mask, SIGTERM) == 0 && sigismember(&mask, SIGCHLD) == 0);
    assert(sigismember(&mask, SIGUSR1) == 1); /* Preserve unrelated original masking. */
    const int signals[] = {SIGINT, SIGTERM, SIGCHLD, SIGPIPE};
    for (size_t i = 0; i < 4; ++i) {
        struct sigaction action; assert(sigaction(signals[i], NULL, &action) == 0);
        assert(action.sa_handler == SIG_DFL);
    }
    return 0;
}

static void test_exec_child_and_restore(void) {
    sigset_t before, block;
    assert(sigprocmask(SIG_SETMASK, NULL, &before) == 0 && sigemptyset(&block) == 0 && sigaddset(&block, SIGUSR1) == 0);
    assert(sigprocmask(SIG_BLOCK, &block, NULL) == 0);
    struct sigaction original; assert(sigaction(SIGTERM, NULL, &original) == 0);
    struct process_signal_wait context; int epoll_fd;
    prepare_fixture(&context, &epoll_fd);
    assert(prepare_process_signal_wait(&context, &signal_state, on_signal) == -1 && errno == EBUSY);
    struct sigaction installed; assert(sigaction(SIGTERM, NULL, &installed) == 0);
    assert(installed.sa_handler == on_signal);
    assert(sigismember(&installed.sa_mask, SIGINT) == 1 && sigismember(&installed.sa_mask, SIGCHLD) == 1);
    assert(sigaction(SIGPIPE, NULL, &installed) == 0 && installed.sa_handler == SIG_IGN);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        close(epoll_fd);
        if (restore_exec_child_signals(&context) < 0) _exit(2);
        execl("/proc/self/exe", "test_process_signal_wait", "--signal-exec-probe", (char *)NULL);
        _exit(3);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert_managed_blocked();
    assert(signal_state == PROCESS_SIGNAL_RUNNING); /* Child changes were private. */
    finish_fixture(&context, epoll_fd);
    struct sigaction restored; assert(sigaction(SIGTERM, NULL, &restored) == 0);
    assert(restored.sa_handler == original.sa_handler);
    sigset_t restored_mask; assert(sigprocmask(SIG_SETMASK, NULL, &restored_mask) == 0 && sigismember(&restored_mask, SIGUSR1) == 1);
    assert(sigprocmask(SIG_SETMASK, &before, NULL) == 0);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--signal-exec-probe")) return exec_probe();
    setvbuf(stdout, NULL, _IOLBF, 0); /* Keep completed groups visible on a later assertion failure. */
    alarm(60); size_t before = open_fd_count();
    test_pending_and_boundary_signals();
    puts("[PASS] pending INT/TERM/CHLD and exact pre-wait boundary: 6 cases");
    test_stop_priority_and_ready_io();
    test_wait_return_contract();
    puts("[PASS] wait readiness/timeout/error, errno preservation, blocked return mask");
    test_async_shutdown_from_child();
    test_multiple_children_caller_reaps();
    puts("[PASS] stop priority, ready I/O, asynchronous shutdown, caller-only multi-child reap");
    test_fatal_before_pipe_wakeup();
    puts("[PASS] SIGCHLD finds READY/WRITING fatal reports before pipe notification");
    test_exec_child_and_restore();
    assert(open_fd_count() == before);
    alarm(0);
    printf("signal context: %zu bytes, no module-owned FD/heap\n", sizeof(struct process_signal_wait));
    puts("[PASS] fork/exec mask/dispositions, parent restoration, FD cleanup");
    return 0;
}
