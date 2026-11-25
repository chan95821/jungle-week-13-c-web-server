#define _POSIX_C_SOURCE 200809L
#include "../../src/shared_backend_state.h"

#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static enum backend_fault_step kill_step;
static unsigned int kill_occurrence;
static unsigned int seen_occurrences;

void shared_backend_fault_step(enum backend_fault_step step) {
    if (step == kill_step && ++seen_occurrences == kill_occurrence) {
        raise(SIGKILL);
        _exit(99);
    }
}

struct fixture {
    struct shared_backend_memory memory;
    uint64_t generation[2];
    struct backend_request_token survivor;
    struct backend_request_token target;
};

static void lock_state(struct shared_backend_memory *memory) {
    struct backend_state_diagnostic diagnostic;
    assert(try_lock_shared_backend_state(memory, &diagnostic) == 0);
}

static void initialize_fixture(struct fixture *fixture, unsigned int target_requests) {
    memset(fixture, 0, sizeof(*fixture));
    assert(create_shared_backend_memory(2, 8, 4096, &fixture->memory) == 0);
    lock_state(&fixture->memory);
    for (uint32_t i = 0; i < 2; ++i)
        assert(activate_backend_worker_locked(&fixture->memory, i, &fixture->generation[i], NULL) == 0);
    assert(reserve_backend_request_locked(&fixture->memory, 1, fixture->generation[1], 3, 1, 0,
                                          &fixture->survivor, NULL) == 0);
    for (uint32_t i = 0; i < target_requests; ++i) {
        struct backend_request_token token;
        assert(reserve_backend_request_locked(&fixture->memory, 0, fixture->generation[0], i,
                                              i == 0 ? 0 : 2, 0, &token, NULL) == 0);
        if (i == 0) fixture->target = token;
    }
    assert(unlock_shared_backend_state(&fixture->memory) == 0);
}

static void wait_for_killed_child(pid_t child) {
    int status;
    assert(child > 0 && waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

static void assert_survivor_and_reclaim(struct fixture *fixture) {
    struct shared_backend_memory *memory = &fixture->memory;
    assert(validate_backend_reservations_locked(memory, NULL) == 0);
    struct backend_request_reservation survivor = memory->reservations[3];
    struct backend_worker_state survivor_worker = memory->workers[1];
    assert(reclaim_backend_worker_locked(memory, 0, fixture->generation[0], NULL) == 0);
    assert(reclaim_backend_worker_locked(memory, 0, fixture->generation[0], NULL) == 0);
    assert(memory->workers[0].state == BACKEND_WORKER_RETIRED);
    assert(memory->header->outstanding[0] == 0 && memory->header->outstanding[1] == 1 &&
           memory->header->outstanding[2] == 0);
    assert(memcmp(&survivor, &memory->reservations[3], sizeof(survivor)) == 0);
    assert(memcmp(&survivor_worker, &memory->workers[1], sizeof(survivor_worker)) == 0);
    assert(validate_backend_reservations_locked(memory, NULL) == 0);
    assert(complete_backend_request_locked(memory, &fixture->survivor, NULL) == 0);
    assert(unlock_shared_backend_state(memory) == 0);
    assert(destroy_shared_backend_memory(memory) == 0);
}

static void test_normal_and_stale_completion(void) {
    struct fixture fixture;
    initialize_fixture(&fixture, 1);
    struct shared_backend_memory *memory = &fixture.memory;
    lock_state(memory);
    assert(complete_backend_request_locked(memory, &fixture.target, NULL) == 0);
    assert(complete_backend_request_locked(memory, &fixture.target, NULL) == 1);
    struct backend_request_token replacement;
    assert(reserve_backend_request_locked(memory, 0, fixture.generation[0], 0, 2, 1, &replacement, NULL) == 0);
    assert(complete_backend_request_locked(memory, &fixture.target, NULL) == 1);
    assert(memory->header->outstanding[2] == 1);
    assert(reclaim_backend_worker_locked(memory, 0, fixture.generation[0], NULL) == 0);
    uint64_t next_generation;
    assert(activate_backend_worker_locked(memory, 0, &next_generation, NULL) == 0);
    assert(next_generation == fixture.generation[0] + 1);
    assert(reserve_backend_request_locked(memory, 0, next_generation, 0, 0, 1, &replacement, NULL) == 0);
    assert(complete_backend_request_locked(memory, &fixture.target, NULL) == 1);
    assert(reclaim_backend_worker_locked(memory, 0, fixture.generation[0], NULL) == 0);
    assert(memory->header->outstanding[0] == 1);
    assert(validate_backend_reservations_locked(memory, NULL) == 0);
    assert(complete_backend_request_locked(memory, &replacement, NULL) == 0);
    fixture.generation[0] = next_generation;
    assert_survivor_and_reclaim(&fixture);
}

static void test_request_generation_overflow(void) {
    struct fixture fixture;
    initialize_fixture(&fixture, 0);
    lock_state(&fixture.memory);
    fixture.memory.reservations[0].request_generation = UINT64_MAX;
    struct backend_request_token token;
    assert(reserve_backend_request_locked(&fixture.memory, 0, fixture.generation[0], 0, 0, 1, &token, NULL) == -1);
    assert(errno == EOVERFLOW && fixture.memory.header->outstanding[0] == 0);
    assert_survivor_and_reclaim(&fixture);
}

static void test_update_crash_matrix(bool completing) {
    for (enum backend_fault_step step = BACKEND_FAULT_JOURNAL_FILLED;
         step <= BACKEND_FAULT_IDLE; ++step) {
        struct fixture fixture;
        initialize_fixture(&fixture, completing ? 1 : 0);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            kill_step = step; kill_occurrence = 1; seen_occurrences = 0;
            lock_state(&fixture.memory);
            if (completing) complete_backend_request_locked(&fixture.memory, &fixture.target, NULL);
            else {
                struct backend_request_token token;
                reserve_backend_request_locked(&fixture.memory, 0, fixture.generation[0], 0, 0, 1, &token, NULL);
            }
            _exit(90); /* Missing checkpoint is a test failure. */
        }
        wait_for_killed_child(child);
        lock_state(&fixture.memory);
        bool committed = step >= BACKEND_FAULT_COMMITTED;
        bool active = completing ? !committed : committed;
        assert(fixture.memory.reservations[0].state ==
               (active ? BACKEND_RESERVATION_ACTIVE : BACKEND_RESERVATION_FREE));
        assert(fixture.memory.header->outstanding[0] == (active ? 1U : 0U));
        assert(fixture.memory.header->next_query_turn == (!completing && committed ? 1U : 0U));
        assert_survivor_and_reclaim(&fixture);
    }
}

static void test_activation_crash_matrix(void) {
    const enum backend_fault_step steps[] = {
        BACKEND_FAULT_JOURNAL_FILLED, BACKEND_FAULT_PREPARED,
        BACKEND_FAULT_WORKER_GENERATION_WRITTEN, BACKEND_FAULT_WORKER_WRITTEN,
        BACKEND_FAULT_COMMITTED, BACKEND_FAULT_IDLE
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        struct fixture fixture;
        initialize_fixture(&fixture, 0);
        lock_state(&fixture.memory);
        assert(reclaim_backend_worker_locked(&fixture.memory, 0, fixture.generation[0], NULL) == 0);
        assert(unlock_shared_backend_state(&fixture.memory) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            kill_step = steps[i]; kill_occurrence = 1; seen_occurrences = 0;
            lock_state(&fixture.memory);
            uint64_t generation;
            activate_backend_worker_locked(&fixture.memory, 0, &generation, NULL);
            _exit(90);
        }
        wait_for_killed_child(child);
        lock_state(&fixture.memory);
        bool committed = steps[i] == BACKEND_FAULT_COMMITTED || steps[i] == BACKEND_FAULT_IDLE;
        assert(fixture.memory.workers[0].generation == fixture.generation[0] + (committed ? 1U : 0U));
        assert(fixture.memory.workers[0].state ==
               (committed ? BACKEND_WORKER_ACTIVE : BACKEND_WORKER_RETIRED));
        fixture.generation[0] = fixture.memory.workers[0].generation;
        assert_survivor_and_reclaim(&fixture);
    }
}
static void test_recovery_crash_matrix(void) {
    for (enum backend_fault_step step = BACKEND_FAULT_RESTORE_RESERVATION;
         step <= BACKEND_FAULT_CONSISTENT; ++step) {
        struct fixture fixture;
        initialize_fixture(&fixture, 0);
        pid_t updater = fork();
        assert(updater >= 0);
        if (updater == 0) {
            kill_step = BACKEND_FAULT_TOTAL_WRITTEN; kill_occurrence = 1; seen_occurrences = 0;
            lock_state(&fixture.memory);
            struct backend_request_token token;
            reserve_backend_request_locked(&fixture.memory, 0, fixture.generation[0], 0, 0, 1, &token, NULL);
            _exit(90);
        }
        wait_for_killed_child(updater);
        pid_t repairer = fork();
        assert(repairer >= 0);
        if (repairer == 0) {
            kill_step = step; kill_occurrence = 1; seen_occurrences = 0;
            lock_state(&fixture.memory);
            _exit(90);
        }
        wait_for_killed_child(repairer);
        lock_state(&fixture.memory);
        assert(fixture.memory.reservations[0].state == BACKEND_RESERVATION_FREE);
        assert(fixture.memory.header->outstanding[0] == 0);
        assert(fixture.memory.header->next_query_turn == 0);
        assert_survivor_and_reclaim(&fixture);
    }
}

static void test_committed_recovery_interruption(void) {
    const enum backend_fault_step initial_steps[] = {BACKEND_FAULT_COMMITTED, BACKEND_FAULT_IDLE};
    const enum backend_fault_step repair_steps[] = {
        BACKEND_FAULT_RECOVERY_VALIDATED, BACKEND_FAULT_RECOVERY_IDLE, BACKEND_FAULT_CONSISTENT
    };
    for (size_t a = 0; a < 2; ++a) {
        for (size_t b = 0; b < 3; ++b) {
            struct fixture fixture;
            initialize_fixture(&fixture, 0);
            pid_t child = fork();
            assert(child >= 0);
            if (child == 0) {
                kill_step = initial_steps[a]; kill_occurrence = 1; seen_occurrences = 0;
                lock_state(&fixture.memory);
                struct backend_request_token token;
                reserve_backend_request_locked(&fixture.memory, 0, fixture.generation[0], 0, 0, 1, &token, NULL);
                _exit(90);
            }
            wait_for_killed_child(child);
            child = fork();
            assert(child >= 0);
            if (child == 0) {
                kill_step = repair_steps[b]; kill_occurrence = 1; seen_occurrences = 0;
                lock_state(&fixture.memory);
                _exit(90);
            }
            wait_for_killed_child(child);
            lock_state(&fixture.memory);
            assert(fixture.memory.reservations[0].state == BACKEND_RESERVATION_ACTIVE);
            assert(fixture.memory.header->outstanding[0] == 1);
            assert(fixture.memory.header->next_query_turn == 1);
            assert_survivor_and_reclaim(&fixture);
        }
    }
}
static void test_reclaim_crash_matrix(void) {
    const enum backend_fault_step steps[] = {
        BACKEND_FAULT_JOURNAL_FILLED, BACKEND_FAULT_PREPARED,
        BACKEND_FAULT_WORKER_WRITTEN, BACKEND_FAULT_COMMITTED, BACKEND_FAULT_IDLE
    };
    /* Four transactions: RETIRING, release first, release second, RETIRED. */
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); ++s) {
        for (unsigned int transaction = 1; transaction <= 4; ++transaction) {
            struct fixture fixture;
            initialize_fixture(&fixture, 2);
            pid_t child = fork();
            assert(child >= 0);
            if (child == 0) {
                kill_step = steps[s]; kill_occurrence = transaction; seen_occurrences = 0;
                lock_state(&fixture.memory);
                reclaim_backend_worker_locked(&fixture.memory, 0, fixture.generation[0], NULL);
                _exit(90);
            }
            wait_for_killed_child(child);
            lock_state(&fixture.memory);
            assert_survivor_and_reclaim(&fixture);
        }
    }
}

static void test_corruption_and_bounded_diagnostic(void) {
    for (unsigned int kind = 0; kind < 6; ++kind) {
        struct fixture fixture;
        initialize_fixture(&fixture, 0);
        lock_state(&fixture.memory);
        if (kind == 0) fixture.memory.header->outstanding[0] = 1;
        if (kind == 1) fixture.memory.reservations[0].backend_index = UINT32_MAX;
        if (kind == 2) fixture.memory.workers[0].outstanding[0] = 1;
        if (kind == 3) fixture.memory.workers[0].state = UINT32_MAX;
        if (kind == 4) fixture.memory.header->worker_capacity = UINT32_MAX;
        if (kind == 5) fixture.memory.header->version = 0;
        struct backend_state_diagnostic diagnostic;
        assert(validate_backend_reservations_locked(&fixture.memory, &diagnostic) == -1 && errno == EUCLEAN);
        assert(diagnostic.failed_check != BACKEND_CHECK_NONE);
        char log_line[256];
        assert(format_backend_state_diagnostic(log_line, sizeof(log_line), &diagnostic) > 0);
        assert(strstr(log_line, "worker=") && strstr(log_line, "generation=") &&
               strstr(log_line, "failed_check=") && strstr(log_line, "recovery_phase=") && strstr(log_line, "reason=") && strstr(log_line, "recovery_stage="));
        char short_buffer[8];
        assert(format_backend_state_diagnostic(short_buffer, sizeof(short_buffer), &diagnostic) == -1 && errno == ENOSPC);
        assert(unlock_shared_backend_state(&fixture.memory) == 0);
        assert(try_lock_shared_backend_state(&fixture.memory, &diagnostic) == -1 && errno == EUCLEAN);
        assert(!fixture.memory.lock_held);
        assert(destroy_shared_backend_memory(&fixture.memory) == 0);
    }
}

static void test_broken_recovery_record(void) {
    for (unsigned int kind = 0; kind < 3; ++kind) {
    struct fixture fixture;
    initialize_fixture(&fixture, 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        kill_step = BACKEND_FAULT_PREPARED; kill_occurrence = 1; seen_occurrences = 0;
        lock_state(&fixture.memory);
        struct backend_request_token token;
        reserve_backend_request_locked(&fixture.memory, 0, fixture.generation[0], 0, 0, 1, &token, NULL);
        _exit(90);
    }
    wait_for_killed_child(child);
    if (kind == 0) fixture.memory.header->recovery.checksum ^= 1;
    if (kind == 1) fixture.memory.header->recovery.reservation_index = UINT32_MAX - 1;
    if (kind == 2) atomic_store(&fixture.memory.header->recovery.phase, 99);
    struct backend_state_diagnostic diagnostic;
    assert(try_lock_shared_backend_state(&fixture.memory, &diagnostic) == -1 && errno == EUCLEAN);
    assert(diagnostic.failed_check == BACKEND_CHECK_JOURNAL);
    assert(try_lock_shared_backend_state(&fixture.memory, &diagnostic) == -1 && errno == EUCLEAN);
    assert(diagnostic.failed_check == BACKEND_CHECK_MUTEX);
    assert(destroy_shared_backend_memory(&fixture.memory) == 0);
    }
}

static void lock_for_contention_test(struct shared_backend_memory *memory) {
    while (try_lock_shared_backend_state(memory, NULL) < 0) {
        assert(errno == EBUSY);
        sched_yield(); /* Test-only contention driver, not the event-loop policy. */
    }
}

static void test_concurrent_updates(void) {
    struct shared_backend_memory memory = {0};
    assert(create_shared_backend_memory(4, 4, 4096, &memory) == 0);
    int start[2], acknowledged[2];
    assert(pipe(start) == 0 && pipe(acknowledged) == 0);
    uint64_t generations[4];
    lock_state(&memory);
    for (uint32_t i = 0; i < 4; ++i)
        assert(activate_backend_worker_locked(&memory, i, &generations[i], NULL) == 0);
    assert(unlock_shared_backend_state(&memory) == 0);
    pid_t children[4];
    for (uint32_t i = 0; i < 4; ++i) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (children[i] == 0) {
            close(start[1]); close(acknowledged[0]);
            char marker;
            assert(read(start[0], &marker, 1) == 1);
            assert(try_lock_shared_backend_state(&memory, NULL) == -1 && errno == EBUSY);
            assert(write(acknowledged[1], "B", 1) == 1);
            close(start[0]); close(acknowledged[1]);
            for (unsigned int request = 0; request < 32; ++request) {
                struct backend_request_token token;
                lock_for_contention_test(&memory);
                assert(reserve_backend_request_locked(&memory, i, generations[i], i, i % 3,
                                                      memory.header->next_query_turn, &token, NULL) == 0);
                assert(unlock_shared_backend_state(&memory) == 0);
                sched_yield();
                lock_for_contention_test(&memory);
                assert(complete_backend_request_locked(&memory, &token, NULL) == 0);
                assert(unlock_shared_backend_state(&memory) == 0);
            }
            _exit(0);
        }
    }
    close(start[0]); close(acknowledged[1]);
    lock_state(&memory);
    assert(write(start[1], "BBBB", 4) == 4);
    close(start[1]);
    for (size_t i = 0; i < 4; ++i) {
        char marker;
        assert(read(acknowledged[0], &marker, 1) == 1 && marker == 'B');
    }
    close(acknowledged[0]);
    assert(unlock_shared_backend_state(&memory) == 0);
    for (size_t i = 0; i < 4; ++i) {
        int status;
        assert(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    lock_state(&memory);
    assert(validate_backend_reservations_locked(&memory, NULL) == 0);
    for (size_t b = 0; b < SHARED_BACKEND_COUNT; ++b) assert(memory.header->outstanding[b] == 0);
    assert(unlock_shared_backend_state(&memory) == 0);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

int main(void) {
    alarm(60); /* Failure containment for the test, not a production timeout. */
    test_normal_and_stale_completion();
    test_request_generation_overflow();
    test_update_crash_matrix(false);
    test_update_crash_matrix(true);
    puts("[PASS] reserve/complete crash boundaries: 8 + 8");
    test_activation_crash_matrix();
    puts("[PASS] worker generation crash boundaries: 6");
    test_recovery_crash_matrix();
    puts("[PASS] PREPARED recovery interrupted and recovered again: 7");
    test_committed_recovery_interruption();
    puts("[PASS] COMMITTED/IDLE recovery interruption preserves commit: 6");
    test_reclaim_crash_matrix();
    puts("[PASS] reclaim transaction boundaries: 20");
    test_corruption_and_bounded_diagnostic();
    test_broken_recovery_record();
    puts("[PASS] corrupt states: 6; corrupt recovery records: 3");
    test_concurrent_updates();
    puts("[PASS] four-process forced contention and 32 reservation/release pairs each");
    alarm(0);
    puts("[PASS] reservations: commit/rollback, recovery interruption, generation reclaim, corruption, contention");
    return 0;
}
