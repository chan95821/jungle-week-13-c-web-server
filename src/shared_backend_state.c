#define _GNU_SOURCE
#include "shared_backend_state.h"

#include <errno.h>
#include <stdalign.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>



static int append_aligned_array(size_t *bytes, size_t alignment,
                                size_t count, size_t item_bytes,
                                size_t *offset);
static int initialize_shared_mutex(pthread_mutex_t *mutex);

int plan_shared_backend_memory(size_t workers, size_t reservations,
                               size_t memory_budget,
                               struct shared_backend_layout *layout) {
    if (!layout) { errno = EINVAL; return -1; }
    *layout = (struct shared_backend_layout){0};
    if (!workers || !reservations || !memory_budget) { errno = EINVAL; return -1; }
    if (workers > UINT32_MAX || reservations > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    struct shared_backend_layout planned = {
        .mapping_bytes = sizeof(struct shared_backend_header),
        .worker_capacity = (uint32_t)workers,
        .reservation_capacity = (uint32_t)reservations
    };
    if (append_aligned_array(&planned.mapping_bytes,
                             alignof(struct backend_worker_state), workers,
                             sizeof(struct backend_worker_state),
                             &planned.worker_offset) < 0 ||
        append_aligned_array(&planned.mapping_bytes,
                             alignof(struct backend_request_reservation), reservations,
                             sizeof(struct backend_request_reservation),
                             &planned.reservation_offset) < 0 ||
        append_aligned_array(&planned.mapping_bytes,
                             alignof(struct shared_worker_failure), workers,
                             sizeof(struct shared_worker_failure),
                             &planned.failure_offset) < 0) return -1;
    if (planned.mapping_bytes > memory_budget) { errno = E2BIG; return -1; }
    *layout = planned;
    return 0;
}

int create_shared_backend_memory(size_t workers, size_t reservations,
                                 size_t memory_budget,
                                 struct shared_backend_memory *memory) {
    if (!memory) { errno = EINVAL; return -1; }
    /* Caller must provide an empty owner: do not overwrite a live mapping. */
    if (memory->header) { errno = EBUSY; return -1; }
    *memory = (struct shared_backend_memory){0};
    struct shared_backend_layout layout;
    if (plan_shared_backend_memory(workers, reservations, memory_budget, &layout) < 0)
        return -1;

    void *mapping = mmap(NULL, layout.mapping_bytes, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return -1;
    struct shared_backend_header *header = mapping;
    atomic_init(&header->recovery.phase, BACKEND_TX_IDLE);
    atomic_init(&header->fatal, 0);
    if (!atomic_is_lock_free(&header->recovery.phase) || !atomic_is_lock_free(&header->fatal)) {
        munmap(mapping, layout.mapping_bytes);
        errno = ENOTSUP;
        return -1;
    }
    struct shared_worker_failure *failures = (void *)((unsigned char *)mapping + layout.failure_offset);
    for (size_t i = 0; i < workers; ++i) {
        atomic_init(&failures[i].publication, WORKER_FAILURE_EMPTY);
        atomic_init(&failures[i].generation, 0);
        if (!atomic_is_lock_free(&failures[i].publication) || !atomic_is_lock_free(&failures[i].generation)) {
            munmap(mapping, layout.mapping_bytes);
            errno = ENOTSUP;
            return -1;
        }
    }
    if (initialize_shared_mutex(&header->mutex) < 0) {
        int saved_errno = errno;
        munmap(mapping, layout.mapping_bytes);
        errno = saved_errno;
        return -1;
    }
    uint64_t *validation_counts = calloc(workers, SHARED_BACKEND_COUNT * sizeof(uint64_t));
    if (!validation_counts) {
        int saved_errno = errno;
        pthread_mutex_destroy(&header->mutex);
        munmap(mapping, layout.mapping_bytes);
        errno = saved_errno;
        return -1;
    }
    header->magic = SHARED_BACKEND_MAGIC;
    header->version = SHARED_BACKEND_VERSION;
    header->worker_capacity = layout.worker_capacity;
    header->reservation_capacity = layout.reservation_capacity;
    unsigned char *bytes = mapping;
    *memory = (struct shared_backend_memory){
        .header = header,
        .workers = (void *)(bytes + layout.worker_offset),
        .reservations = (void *)(bytes + layout.reservation_offset),
        .worker_failures = failures,
        .layout = layout,
        .mutex_initialized = true,
        .validation_counts = validation_counts
    };
    return 0;
}

int destroy_shared_backend_memory(struct shared_backend_memory *memory) {
    if (!memory) { errno = EINVAL; return -1; }
    if (!memory->header) return 0;
    if (memory->lock_held) { errno = EBUSY; return -1; }
    if (memory->mutex_initialized) {
        int result = pthread_mutex_destroy(&memory->header->mutex);
        if (result) { errno = result; return -1; }
        memory->mutex_initialized = false;
    }
    /* Bounds come from the process-local owner, not mutable shared metadata. */
    if (munmap(memory->header, memory->layout.mapping_bytes) < 0) return -1;
    free(memory->validation_counts);
    *memory = (struct shared_backend_memory){0};
    return 0;
}

static int append_aligned_array(size_t *bytes, size_t alignment,
                                size_t count, size_t item_bytes,
                                size_t *offset) {
    size_t padding = (alignment - (*bytes % alignment)) % alignment;
    if (*bytes > SIZE_MAX - padding) { errno = EOVERFLOW; return -1; }
    size_t start = *bytes + padding;
    if (count > (SIZE_MAX - start) / item_bytes) { errno = EOVERFLOW; return -1; }
    *offset = start;
    *bytes = start + count * item_bytes;
    return 0;
}

static int initialize_shared_mutex(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attr;
    int result = pthread_mutexattr_init(&attr);
    if (result) { errno = result; return -1; }
    result = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (!result) result = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    if (!result) result = pthread_mutex_init(mutex, &attr);
    int cleanup_result = pthread_mutexattr_destroy(&attr);
    if (!result && cleanup_result) {
        pthread_mutex_destroy(mutex);
        result = cleanup_result;
    }
    if (result) { errno = result; return -1; }
    return 0;
}
