#define _GNU_SOURCE
#include "../../src/worker_failure_channel.h"
#include "../../src/worker_fd_channel.h"
#include "../../src/concurrency_limits.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static enum worker_failure_fault_step kill_step;
static int next_write_error, next_read_error;

void worker_failure_fault_step(enum worker_failure_fault_step step) {
    if (step == kill_step) { raise(SIGKILL); _exit(99); }
}
ssize_t worker_failure_test_write(int fd, const void *buffer, size_t bytes) {
    if (next_write_error) { errno = next_write_error; next_write_error = 0; return -1; }
    return write(fd, buffer, bytes);
}
ssize_t worker_failure_test_read(int fd, void *buffer, size_t bytes) {
    if (next_read_error) { errno = next_read_error; next_read_error = 0; return -1; }
    return read(fd, buffer, bytes);
}

static size_t open_fd_count(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++count;
    assert(closedir(directory) == 0);
    return count;
}

static struct backend_state_diagnostic sample_diagnostic(void) {
    return (struct backend_state_diagnostic){
        .worker_index = 1, .worker_generation = 9, .failed_check = BACKEND_CHECK_TOTAL,
        .recovery_phase = BACKEND_TX_PREPARED, .recovery_stage = BACKEND_STAGE_VALIDATE
    };
}

static void initialize_fixture(struct shared_backend_memory *memory, size_t workers) {
    *memory = (struct shared_backend_memory){0};
    assert(create_shared_backend_memory(workers, 4, 4096, memory) == 0);
    for (uint32_t i = 0; i < workers; ++i) assert(arm_worker_failure_slot(memory, i, 1) == 0);
}

static int create_epoll_for_pipe(int read_fd) {
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC); assert(epoll_fd >= 0);
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 17}; /* LT: no EPOLLET. */
    assert(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, read_fd, &event) == 0);
    return epoll_fd;
}

static void assert_ready_report(struct shared_backend_memory *memory, uint32_t worker, uint64_t generation) {
    struct worker_failure_report report;
    assert(read_worker_failure_report(memory, worker, generation, &report) == WORKER_FAILURE_REPORT_READY);
    assert(report.reporter_index == worker && report.reporter_generation == generation);
    assert(report.reason == WORKER_FAILURE_SHARED_STATE);
    assert(report.diagnostic.failed_check == BACKEND_CHECK_TOTAL);
    assert(report.diagnostic.worker_index == 1 && report.diagnostic.worker_generation == 9);
}

static void test_publication_wakeup_and_first_report(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 2);
    int wakeup[2]; assert(create_worker_failure_pipe(wakeup) == 0);
    for (size_t i = 0; i < 2; ++i) {
        assert(fcntl(wakeup[i], F_GETFL) & O_NONBLOCK);
        assert(fcntl(wakeup[i], F_GETFD) & FD_CLOEXEC);
    }
    int epoll_fd = create_epoll_for_pipe(wakeup[0]);
    struct epoll_event event;
    assert(epoll_wait(epoll_fd, &event, 1, 0) == 0);
    struct worker_failure_report output;
    memset(&output, 0x5a, sizeof(output));
    unsigned char original[sizeof(output)]; memcpy(original, &output, sizeof(output));
    assert(read_worker_failure_report(&memory, 0, 1, &output) == WORKER_FAILURE_NO_REPORT);
    assert(!memcmp(original, &output, sizeof(output)));
    struct backend_state_diagnostic diagnostic = sample_diagnostic();
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 0);
    assert(atomic_load(&memory.header->fatal) == 1);
    assert(epoll_wait(epoll_fd, &event, 1, 0) == 0); /* Shared memory alone is not a wakeup. */
    assert_ready_report(&memory, 0, 1);
    diagnostic.failed_check = BACKEND_CHECK_HEADER;
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 1);
    assert_ready_report(&memory, 0, 1);
    assert(arm_worker_failure_slot(&memory, 0, 2) == -1 && errno == EUCLEAN);
    assert_ready_report(&memory, 0, 1);
    assert(notify_parent_worker_failure(wakeup[1]) == WORKER_FAILURE_NOTIFIED);
    assert(epoll_wait(epoll_fd, &event, 1, 0) == 1 && event.data.u64 == 17);
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_WAKEUP_RECEIVED);
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_NO_WAKEUP);
    assert(read_worker_failure_report(&memory, 0, 1, &output) == WORKER_FAILURE_REPORT_READY);
    char line[WORKER_FAILURE_LOG_BYTES];
    assert(format_worker_failure_report(line, sizeof(line), &output) > 0);
    assert(strstr(line, "reporter=") && strstr(line, "generation=") && strstr(line, "failed_check=") &&
           strstr(line, "recovery_phase=") && strstr(line, "recovery_stage=") && strstr(line, "reason="));
    char short_line[8];
    assert(format_worker_failure_report(short_line, sizeof(short_line), &output) == -1 && errno == ENOSPC);
    close(wakeup[0]); close(wakeup[1]); close(epoll_fd);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_generations_and_invalid_arguments(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 1);
    assert(arm_worker_failure_slot(&memory, 0, 1) == -1 && errno == ESTALE);
    pid_t old_worker = fork(); assert(old_worker >= 0);
    if (old_worker == 0) _exit(0); /* Ordinary exit, no fatal publication. */
    int status; assert(waitpid(old_worker, &status, 0) == old_worker && WIFEXITED(status));
    assert(arm_worker_failure_slot(&memory, 0, 2) == 0);
    struct backend_state_diagnostic diagnostic = sample_diagnostic();
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == -1 && errno == ESTALE);
    assert(publish_worker_failure(&memory, 1, 2, &diagnostic) == -1 && errno == EINVAL);
    struct worker_failure_report output;
    memset(&output, 0x5a, sizeof(output));
    unsigned char original[sizeof(output)]; memcpy(original, &output, sizeof(output));
    assert(read_worker_failure_report(&memory, 0, 1, &output) == WORKER_FAILURE_REPORT_STALE);
    assert(!memcmp(original, &output, sizeof(output)) && atomic_load(&memory.header->fatal) == 0);
    assert(publish_worker_failure(&memory, 0, 2, &diagnostic) == 0);
    assert_ready_report(&memory, 0, 2);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_pipe_full_and_lt_readiness(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 1);
    int wakeup[2]; assert(create_worker_failure_pipe(wakeup) == 0);
    struct backend_state_diagnostic diagnostic = sample_diagnostic();
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 0);
    unsigned char padding[1024] = {0}; bool full = false;
    for (size_t i = 0; i < 256; ++i) {
        ssize_t count = write(wakeup[1], padding, sizeof(padding));
        if (count < 0) { assert(errno == EAGAIN); full = true; break; }
        assert(count > 0);
    }
    assert(full);
    int epoll_fd = create_epoll_for_pipe(wakeup[0]); struct epoll_event event;
    assert(notify_parent_worker_failure(wakeup[1]) == WORKER_FAILURE_ALREADY_READABLE);
    assert(epoll_wait(epoll_fd, &event, 1, 0) == 1);
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_WAKEUP_RECEIVED);
    assert(epoll_wait(epoll_fd, &event, 1, 0) == 1); /* LT repeats while unread bytes remain. */
    bool empty = false;
    for (size_t i = 0; i < 4096; ++i) {
        int result = consume_worker_failure_wakeup(wakeup[0]);
        if (result == WORKER_FAILURE_NO_WAKEUP) { empty = true; break; }
        assert(result == WORKER_FAILURE_WAKEUP_RECEIVED);
    }
    assert(empty && atomic_load(&memory.header->fatal) == 1);
    assert_ready_report(&memory, 0, 1);
    close(epoll_fd); close(wakeup[0]); close(wakeup[1]);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_eintr_epipe_and_eof(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 1);
    int wakeup[2]; assert(create_worker_failure_pipe(wakeup) == 0);
    struct backend_state_diagnostic diagnostic = sample_diagnostic();
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 0);
    next_write_error = EINTR;
    assert(notify_parent_worker_failure(wakeup[1]) == WORKER_FAILURE_NOTIFY_RETRY);
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_NO_WAKEUP);
    assert(notify_parent_worker_failure(wakeup[1]) == WORKER_FAILURE_NOTIFIED);
    next_read_error = EINTR;
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_READ_RETRY);
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_WAKEUP_RECEIVED);
    close(wakeup[0]);
    assert(notify_parent_worker_failure(wakeup[1]) == -1 && errno == EPIPE);
    assert_ready_report(&memory, 0, 1);
    close(wakeup[1]);
    assert(create_worker_failure_pipe(wakeup) == 0);
    close(wakeup[1]);
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_WAKEUP_EOF);
    close(wakeup[0]);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_stream_congestion_does_not_block_wakeup(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 1);
    int wakeup[2], channel[2];
    assert(create_worker_failure_pipe(wakeup) == 0 && create_worker_fd_channel(channel) == 0);
    int buffer_size = 4096;
    assert(setsockopt(channel[1], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == 0);
    unsigned char padding[1024] = {0}; bool full = false;
    for (size_t i = 0; i < 256; ++i) {
        ssize_t count = send(channel[1], padding, sizeof(padding), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0) { assert(errno == EAGAIN); full = true; break; }
        assert(count > 0);
    }
    assert(full);
    struct backend_state_diagnostic diagnostic = sample_diagnostic();
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 0);
    assert(notify_parent_worker_failure(wakeup[1]) == WORKER_FAILURE_NOTIFIED);
    int epoll_fd = create_epoll_for_pipe(wakeup[0]); struct epoll_event event;
    assert(epoll_wait(epoll_fd, &event, 1, 0) == 1 && event.data.u64 == 17);
    /* The congested STREAM is deliberately not drained. */
    assert_ready_report(&memory, 0, 1);
    close(epoll_fd); close(wakeup[0]); close(wakeup[1]); close(channel[0]); close(channel[1]);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_publication_death_boundaries(void) {
    for (enum worker_failure_fault_step step = WORKER_FAILURE_INTENT_MARKED;
         step <= WORKER_FAILURE_PIPE_WRITTEN; ++step) {
        struct shared_backend_memory memory; initialize_fixture(&memory, 2);
        int wakeup[2]; assert(create_worker_failure_pipe(wakeup) == 0);
        pid_t child = fork(); assert(child >= 0);
        if (child == 0) {
            close(wakeup[0]); kill_step = step;
            struct backend_state_diagnostic diagnostic = sample_diagnostic();
            if (publish_worker_failure(&memory, 0, 1, &diagnostic) < 0) _exit(2);
            if (notify_parent_worker_failure(wakeup[1]) < 0) _exit(3);
            _exit(90); /* Must have hit the requested kill checkpoint. */
        }
        int status;
        assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        /* Parent retains a writer to model other live workers: early deaths
         * cannot accidentally appear as EOF wakeups. SIGCHLD handling is needed. */
        assert(atomic_load(&memory.header->fatal) == 1);
        struct worker_failure_report output;
        memset(&output, 0x5a, sizeof(output));
        unsigned char original[sizeof(output)]; memcpy(original, &output, sizeof(output));
        int result = read_worker_failure_report(&memory, 0, 1, &output);
        if (step == WORKER_FAILURE_INTENT_MARKED) assert(result == WORKER_FAILURE_NO_REPORT);
        else if (step < WORKER_FAILURE_PUBLISHED) assert(result == WORKER_FAILURE_REPORT_INCOMPLETE);
        else assert_ready_report(&memory, 0, 1);
        if (step < WORKER_FAILURE_PUBLISHED) {
            assert(!memcmp(original, &output, sizeof(output)));
            char line[WORKER_FAILURE_LOG_BYTES];
            assert(format_missing_worker_failure_report(line, sizeof(line), 0, 1, result) > 0);
            assert(strstr(line, "failed_check=unknown") && strstr(line, "recovery_stage=unknown"));
            if (step == WORKER_FAILURE_INTENT_MARKED) assert(strstr(line, "reason=not_published"));
            else assert(strstr(line, "reason=incomplete_publication"));
        }
        int wake_result = consume_worker_failure_wakeup(wakeup[0]);
        assert(wake_result == (step == WORKER_FAILURE_PIPE_WRITTEN ?
                               WORKER_FAILURE_WAKEUP_RECEIVED : WORKER_FAILURE_NO_WAKEUP));
        close(wakeup[0]); close(wakeup[1]);
        assert(destroy_shared_backend_memory(&memory) == 0);
    }
}

static void test_concurrent_reports(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 4);
    int wakeup[2], start[2]; assert(create_worker_failure_pipe(wakeup) == 0 && pipe(start) == 0);
    pid_t children[4];
    for (uint32_t i = 0; i < 4; ++i) {
        children[i] = fork(); assert(children[i] >= 0);
        if (children[i] == 0) {
            close(start[1]); close(wakeup[0]); char marker;
            if (read(start[0], &marker, 1) != 1) _exit(2);
            struct backend_state_diagnostic diagnostic = sample_diagnostic();
            if (publish_worker_failure(&memory, i, 1, &diagnostic) != 0) _exit(3);
            if (notify_parent_worker_failure(wakeup[1]) < 0) _exit(4);
            _exit(0);
        }
    }
    close(start[0]); assert(write(start[1], "RRRR", 4) == 4); close(start[1]);
    for (size_t i = 0; i < 4; ++i) {
        int status; assert(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert_ready_report(&memory, (uint32_t)i, 1);
    }
    assert(consume_worker_failure_wakeup(wakeup[0]) == WORKER_FAILURE_WAKEUP_RECEIVED);
    close(wakeup[0]); close(wakeup[1]);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_corrupt_report_detection(void) {
    for (unsigned int kind = 0; kind < 5; ++kind) {
        struct shared_backend_memory memory; initialize_fixture(&memory, 1);
        struct backend_state_diagnostic diagnostic = sample_diagnostic();
        if (kind >= 2) assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 0);
        struct shared_worker_failure *slot = &memory.worker_failures[0];
        if (kind == 0) atomic_store(&slot->publication, 99);
        if (kind == 1) slot->reserved = 1;
        if (kind == 2) slot->report.reporter_index = UINT32_MAX;
        if (kind == 3) slot->report.reason = 99;
        if (kind == 4) slot->report.reporter_generation = 2;
        struct worker_failure_report output;
        memset(&output, 0x5a, sizeof(output));
        unsigned char original[sizeof(output)]; memcpy(original, &output, sizeof(output));
        assert(read_worker_failure_report(&memory, 0, 1, &output) == -1 && errno == EUCLEAN);
        assert(!memcmp(original, &output, sizeof(output)) && atomic_load(&memory.header->fatal) == 1);
        char line[WORKER_FAILURE_LOG_BYTES];
        assert(format_missing_worker_failure_report(line, sizeof(line), 0, 1, -1) > 0);
        assert(strstr(line, "reason=untrusted_report") && strstr(line, "failed_check=unknown"));
        assert(destroy_shared_backend_memory(&memory) == 0);
    }
}

static void test_backend_failure_to_report(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 1);
    struct backend_state_diagnostic diagnostic;
    assert(try_lock_shared_backend_state(&memory, &diagnostic) == 0);
    memory.header->outstanding[0] = 1; /* Isolated invariant fault, no active requests. */
    assert(validate_backend_reservations_locked(&memory, &diagnostic) == -1 && errno == EUCLEAN);
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == -1 && errno == EPERM);
    assert(unlock_shared_backend_state(&memory) == 0);
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 0);
    struct worker_failure_report report;
    assert(read_worker_failure_report(&memory, 0, 1, &report) == WORKER_FAILURE_REPORT_READY);
    assert(report.diagnostic.failed_check == BACKEND_CHECK_TOTAL);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_report_without_recoverable_mutex(void) {
    struct shared_backend_memory memory; initialize_fixture(&memory, 1);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        if (pthread_mutex_lock(&memory.header->mutex)) _exit(2);
        _exit(0); /* Owner dies with mutex held. */
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(pthread_mutex_trylock(&memory.header->mutex) == EOWNERDEAD);
    assert(pthread_mutex_unlock(&memory.header->mutex) == 0); /* Deliberately unrecoverable. */
    struct backend_state_diagnostic diagnostic;
    assert(try_lock_shared_backend_state(&memory, &diagnostic) == -1 && errno == EUCLEAN);
    assert(diagnostic.failed_check == BACKEND_CHECK_MUTEX && !memory.lock_held);
    assert(publish_worker_failure(&memory, 0, 1, &diagnostic) == 0);
    struct worker_failure_report report;
    assert(read_worker_failure_report(&memory, 0, 1, &report) == WORKER_FAILURE_REPORT_READY);
    assert(report.diagnostic.failed_check == BACKEND_CHECK_MUTEX);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

int main(void) {
    struct sigaction ignore = {0}, previous;
    ignore.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignore.sa_mask) == 0 && sigaction(SIGPIPE, &ignore, &previous) == 0);
    alarm(60);
    size_t before = open_fd_count();
    test_publication_wakeup_and_first_report();
    test_generations_and_invalid_arguments();
    test_pipe_full_and_lt_readiness();
    test_eintr_epipe_and_eof();
    test_stream_congestion_does_not_block_wakeup();
    puts("[PASS] separate pipe: LT wakeup, full pipe/STREAM, EINTR/EPIPE/EOF, generation and first report");
    test_publication_death_boundaries();
    puts("[PASS] SIGKILL at 5 publication/wakeup boundaries; no incomplete payload read");
    test_concurrent_reports();
    test_corrupt_report_detection();
    test_backend_failure_to_report();
    test_report_without_recoverable_mutex();
    assert(open_fd_count() == before);
    struct shared_backend_layout layout;
    assert(plan_shared_backend_memory(INITIAL_WORKER_COUNT, INITIAL_BACKEND_RESERVATION_CAPACITY,
                                      INITIAL_SHARED_MEMORY_BUDGET, &layout) == 0);
    printf("failure storage: slot=%zu report=%zu initial mapping=%zu bytes\n",
           sizeof(struct shared_worker_failure), sizeof(struct worker_failure_report), layout.mapping_bytes);
    alarm(0);
    assert(sigaction(SIGPIPE, &previous, NULL) == 0);
    puts("[PASS] concurrent reports, corruption, fatal backend/unrecoverable mutex, FD cleanup");
    return 0;
}
