#define _POSIX_C_SOURCE 200809L
#include "parent_client_queue.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static bool valid_queue(const struct parent_client_queue *queue);

int create_parent_client_queue(size_t capacity, struct parent_client_queue *queue) {
    if (!queue || !capacity) { errno = EINVAL; return -1; }
    if (queue->clients) { errno = EBUSY; return -1; }
    if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(struct parent_queued_client)) {
        errno = EOVERFLOW;
        return -1;
    }
    struct parent_queued_client *clients = calloc(capacity, sizeof(*clients));
    if (!clients) return -1;
    for (size_t i = 0; i < capacity; ++i) clients[i].client_fd = -1;
    *queue = (struct parent_client_queue){.clients = clients, .capacity = capacity};
    return 0;
}

bool parent_accept_has_capacity(const struct parent_client_queue *queue,
                                size_t global_connections, size_t global_limit,
                                bool stopping) {
    return !stopping && valid_queue(queue) && queue->count < queue->capacity &&
           global_connections < global_limit;
}

int enqueue_parent_client(struct parent_client_queue *queue, int *client_fd,
                           uint64_t connection_id, const struct timespec *accepted_at) {
    if (!valid_queue(queue) || !client_fd || *client_fd < 0 || !connection_id ||
        !accepted_at || accepted_at->tv_sec < 0 || accepted_at->tv_nsec < 0 ||
        accepted_at->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
    if (queue->count == queue->capacity) { errno = ENOSPC; return -1; }
    /* The allocation overflow bound also makes head+count safe. */
    size_t tail = (queue->head + queue->count) % queue->capacity;
    queue->clients[tail] = (struct parent_queued_client){*client_fd, connection_id, *accepted_at};
    ++queue->count;
    *client_fd = -1;
    return 0;
}

int dequeue_parent_client(struct parent_client_queue *queue, struct parent_queued_client *client) {
    if (!valid_queue(queue) || !client) { errno = EINVAL; return -1; }
    if (client->client_fd >= 0) { errno = EBUSY; return -1; }
    if (!queue->count) return 0;
    *client = queue->clients[queue->head];
    queue->clients[queue->head] = (struct parent_queued_client){.client_fd = -1};
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return 1;
}

int destroy_parent_client_queue(struct parent_client_queue *queue) {
    if (!queue) { errno = EINVAL; return -1; }
    if (!queue->clients) { *queue = (struct parent_client_queue){0}; return 0; }
    if (!valid_queue(queue)) { errno = EINVAL; return -1; }
    int first_error = 0;
    while (queue->count) {
        struct parent_queued_client client = {.client_fd = -1};
        if (dequeue_parent_client(queue, &client) < 0) return -1;
        /* Linux: do not retry close(EINTR); continue releasing other entries. */
        if (close(client.client_fd) < 0 && !first_error) first_error = errno;
    }
    free(queue->clients);
    *queue = (struct parent_client_queue){0};
    if (first_error) { errno = first_error; return -1; }
    return 0;
}

static bool valid_queue(const struct parent_client_queue *queue) {
    return queue && queue->clients && queue->capacity &&
           queue->capacity <= SIZE_MAX / sizeof(struct parent_queued_client) &&
           queue->head < queue->capacity && queue->count <= queue->capacity;
}
