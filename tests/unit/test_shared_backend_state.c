#define _POSIX_C_SOURCE 200809L
#include "../../src/shared_backend_state.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdalign.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_layout_limits(void) {
    struct shared_backend_layout layout;
    assert(plan_shared_backend_memory(0, 1, SIZE_MAX, &layout) == -1 && errno == EINVAL);
    assert(layout.mapping_bytes == 0);
    assert(plan_shared_backend_memory(1, 0, SIZE_MAX, &layout) == -1 && errno == EINVAL);
    assert(plan_shared_backend_memory(1, 1, 0, &layout) == -1 && errno == EINVAL);
    assert(plan_shared_backend_memory(SIZE_MAX, 1, SIZE_MAX, &layout) == -1 && errno == EOVERFLOW);
    assert(plan_shared_backend_memory(1, SIZE_MAX, SIZE_MAX, &layout) == -1 && errno == EOVERFLOW);
    assert(plan_shared_backend_memory(4, 10000, SIZE_MAX, &layout) == 0);
    size_t bytes = layout.mapping_bytes;
    assert(layout.worker_offset % alignof(struct backend_worker_state) == 0);
    assert(layout.reservation_offset % alignof(struct backend_request_reservation) == 0);
    assert(layout.failure_offset % alignof(struct shared_worker_failure) == 0);
    assert(plan_shared_backend_memory(4, 10000, bytes - 1, &layout) == -1 && errno == E2BIG);
    assert(layout.mapping_bytes == 0);
    assert(plan_shared_backend_memory(4, 10000, bytes, &layout) == 0);
    printf("layout example only: header=%zu worker=%zu reservation=%zu journal=%zu total=%zu\n",
           sizeof(struct shared_backend_header), sizeof(struct backend_worker_state),
           sizeof(struct backend_request_reservation), sizeof(struct backend_recovery_record), bytes);
}

static void test_initialization(void) {
    struct shared_backend_memory memory = {0};
    assert(create_shared_backend_memory(2, 4, 1, &memory) == -1 && errno == E2BIG);
    assert(memory.header == NULL);
    assert(create_shared_backend_memory(2, 4, 4096, &memory) == 0);
    assert(memory.layout.mapping_bytes <= 4096);
    assert(memory.header->worker_capacity == 2 && memory.header->reservation_capacity == 4);
    assert(memory.header->version == SHARED_BACKEND_VERSION);
    assert(atomic_load(&memory.header->recovery.phase) == 0);
    for (size_t worker = 0; worker < 2; ++worker) {
        assert(memory.workers[worker].generation == 0);
        assert(atomic_load(&memory.worker_failures[worker].generation) == 0);
        assert(atomic_load(&memory.worker_failures[worker].publication) == WORKER_FAILURE_EMPTY);
        for (size_t backend = 0; backend < SHARED_BACKEND_COUNT; ++backend)
            assert(memory.workers[worker].outstanding[backend] == 0);
    }
    for (size_t request = 0; request < 4; ++request)
        assert(memory.reservations[request].state == BACKEND_RESERVATION_FREE);
    void *original = memory.header;
    assert(create_shared_backend_memory(2, 4, 4096, &memory) == -1 && errno == EBUSY);
    assert(memory.header == original);
    assert(destroy_shared_backend_memory(&memory) == 0);
    assert(memory.header == NULL && memory.layout.mapping_bytes == 0);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

static void test_shared_mutex_owner_death(void) {
    struct shared_backend_memory memory = {0};
    assert(create_shared_backend_memory(2, 4, 4096, &memory) == 0);
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        if (pthread_mutex_trylock(&memory.header->mutex) != 0) _exit(2);
        /* Test-only marker proves mmap sharing; no application transaction. */
        memory.header->reserved = 77;
        char marker = 'R';
        if (write(ready[1], &marker, 1) != 1) _exit(3);
        for (;;) pause();
    }
    close(ready[1]);
    char marker = 0;
    assert(read(ready[0], &marker, 1) == 1 && marker == 'R');
    close(ready[0]);
    assert(kill(child, SIGKILL) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(pthread_mutex_trylock(&memory.header->mutex) == EOWNERDEAD);
    assert(memory.header->reserved == 77);
    /* No interrupted ledger update exists here; reset only our test marker. */
    memory.header->reserved = 0;
    assert(pthread_mutex_consistent(&memory.header->mutex) == 0);
    assert(pthread_mutex_unlock(&memory.header->mutex) == 0);
    assert(pthread_mutex_trylock(&memory.header->mutex) == 0);
    assert(pthread_mutex_unlock(&memory.header->mutex) == 0);
    assert(destroy_shared_backend_memory(&memory) == 0);
}

int main(void) {
    alarm(10); /* Test hang guard, not a production recovery timeout. */
    test_layout_limits();
    test_initialization();
    test_shared_mutex_owner_death();
    alarm(0);
    puts("[PASS] shared backend initialization and mutex owner-death primitive");
    return 0;
}
