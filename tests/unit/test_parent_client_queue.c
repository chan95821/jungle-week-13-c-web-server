#define _GNU_SOURCE
#include "../../src/parent_client_queue.h"
#include "../../src/concurrency_limits.h"
#include "../../src/worker_fd_channel.h"
#include "../../src/socket_io.h"

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t open_fd_count(void) {
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++count;
    assert(closedir(directory) == 0);
    return count;
}

static void make_client(int client[2]) {
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, client) == 0);
}

static void test_creation_and_invalid_input(void) {
    struct parent_client_queue queue = {0};
    assert(create_parent_client_queue(0, &queue) == -1 && errno == EINVAL);
    assert(create_parent_client_queue(SIZE_MAX, &queue) == -1 && errno == EOVERFLOW);
    assert(!queue.clients);
    assert(create_parent_client_queue(1, &queue) == 0);
    void *owner = queue.clients;
    assert(create_parent_client_queue(1, &queue) == -1 && errno == EBUSY && queue.clients == owner);
    int client[2]; make_client(client);
    struct timespec accepted = {123, 1000000000L};
    int original = client[0];
    assert(enqueue_parent_client(&queue, &client[0], 1, &accepted) == -1 && errno == EINVAL);
    assert(client[0] == original && queue.count == 0);
    struct parent_queued_client output = {.client_fd = -1};
    assert(dequeue_parent_client(&queue, &output) == 0);
    output.client_fd = client[0];
    assert(dequeue_parent_client(&queue, &output) == -1 && errno == EBUSY);
    assert(fcntl(client[0], F_GETFD) >= 0);
    close(client[0]); close(client[1]);
    assert(destroy_parent_client_queue(&queue) == 0);
    assert(destroy_parent_client_queue(&queue) == 0);
}

static void test_default_capacity_and_wrap(void) {
    size_t before = open_fd_count();
    struct parent_client_queue queue = {0};
    assert(create_parent_client_queue(INITIAL_PARENT_QUEUE_CAPACITY, &queue) == 0);
    int peers[INITIAL_PARENT_QUEUE_CAPACITY + 1];
    for (size_t i = 0; i < INITIAL_PARENT_QUEUE_CAPACITY; ++i) {
        int client[2]; make_client(client);
        peers[i] = client[1];
        struct timespec accepted = {(time_t)(i + 100), (long)i};
        assert(enqueue_parent_client(&queue, &client[0], i + 1, &accepted) == 0);
        assert(client[0] == -1);
        assert(queue.count == i + 1);
        if (i + 1 == INITIAL_PARENT_QUEUE_CAPACITY - 1)
            assert(parent_accept_has_capacity(&queue, i + 1, INITIAL_TOTAL_CONNECTION_LIMIT, false));
    }
    assert(!parent_accept_has_capacity(&queue, queue.count, INITIAL_TOTAL_CONNECTION_LIMIT, false));
    int extra[2]; make_client(extra);
    int original = extra[0]; struct timespec accepted = {999, 1};
    assert(enqueue_parent_client(&queue, &extra[0], 999, &accepted) == -1 && errno == ENOSPC);
    assert(extra[0] == original && fcntl(original, F_GETFD) >= 0);
    close(extra[0]); close(extra[1]);

    struct parent_queued_client first = {.client_fd = -1};
    assert(dequeue_parent_client(&queue, &first) == 1 && first.connection_id == 1);
    assert(first.accepted_at.tv_sec == 100 && first.accepted_at.tv_nsec == 0);
    assert(parent_accept_has_capacity(&queue, INITIAL_PARENT_QUEUE_CAPACITY, INITIAL_TOTAL_CONNECTION_LIMIT, false));
    make_client(extra); peers[INITIAL_PARENT_QUEUE_CAPACITY] = extra[1];
    accepted = (struct timespec){999, 1};
    assert(enqueue_parent_client(&queue, &extra[0], INITIAL_PARENT_QUEUE_CAPACITY + 1, &accepted) == 0);
    for (size_t id = 2; id <= INITIAL_PARENT_QUEUE_CAPACITY + 1; ++id) {
        struct parent_queued_client output = {.client_fd = -1};
        assert(dequeue_parent_client(&queue, &output) == 1 && output.connection_id == id);
        if (id <= INITIAL_PARENT_QUEUE_CAPACITY) {
            assert(output.accepted_at.tv_sec == (time_t)(id + 99));
            assert(output.accepted_at.tv_nsec == (long)(id - 1));
        }
        close(output.client_fd);
    }
    assert(queue.count == 0);
    assert(destroy_parent_client_queue(&queue) == 0);
    char byte;
    /* Destroy cannot close the FD already moved out of the queue. */
    assert(recv(peers[0], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(first.client_fd);
    for (size_t i = 0; i < INITIAL_PARENT_QUEUE_CAPACITY + 1; ++i) {
        assert(recv(peers[i], &byte, 1, MSG_DONTWAIT) == 0);
        close(peers[i]);
    }
    assert(open_fd_count() == before);
    printf("queue storage example: item=%zu owner=%zu entries=%zu\n",
           sizeof(struct parent_queued_client), sizeof(struct parent_client_queue),
           sizeof(struct parent_queued_client) * (size_t)INITIAL_PARENT_QUEUE_CAPACITY);
}

static void test_destroy_pending_and_close_error(void) {
    size_t before = open_fd_count();
    for (unsigned int inject = 0; inject < 2; ++inject) {
        struct parent_client_queue queue = {0}; assert(create_parent_client_queue(2, &queue) == 0);
        int peers[2];
        for (size_t i = 0; i < 2; ++i) {
            int client[2]; make_client(client); peers[i] = client[1];
            struct timespec accepted = {1, 2};
            assert(enqueue_parent_client(&queue, &client[0], i + 1, &accepted) == 0);
        }
        /* Isolated fault: simulate an owned FD having already been closed.
         * No allocation/reuse occurs before destroy sees EBADF. */
        if (inject) close(queue.clients[queue.head].client_fd);
        int result = destroy_parent_client_queue(&queue);
        if (inject) assert(result == -1 && errno == EBADF);
        else assert(result == 0);
        assert(!queue.clients && queue.count == 0);
        assert(destroy_parent_client_queue(&queue) == 0);
        char byte;
        for (size_t i = 0; i < 2; ++i) {
            assert(recv(peers[i], &byte, 1, MSG_DONTWAIT) == 0);
            close(peers[i]);
        }
        assert(open_fd_count() == before);
    }
}

static void test_global_and_stopping_gate(void) {
    struct parent_client_queue queue = {0}; assert(create_parent_client_queue(1, &queue) == 0);
    assert(parent_accept_has_capacity(&queue, INITIAL_TOTAL_CONNECTION_LIMIT - 1,
                                     INITIAL_TOTAL_CONNECTION_LIMIT, false));
    assert(!parent_accept_has_capacity(&queue, INITIAL_TOTAL_CONNECTION_LIMIT,
                                      INITIAL_TOTAL_CONNECTION_LIMIT, false));
    assert(!parent_accept_has_capacity(&queue, INITIAL_TOTAL_CONNECTION_LIMIT + 1,
                                      INITIAL_TOTAL_CONNECTION_LIMIT, false));
    assert(!parent_accept_has_capacity(&queue, 0, INITIAL_TOTAL_CONNECTION_LIMIT, true));
    assert(!parent_accept_has_capacity(&queue, 0, 0, false));
    assert(destroy_parent_client_queue(&queue) == 0);
}

static int connect_fixture_client(const struct sockaddr_in *address) {
    int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); assert(client >= 0);
    assert(connect(client, (const struct sockaddr *)address, sizeof(*address)) == 0);
    return client;
}

static void test_real_listener_pause_and_resume(void) {
    size_t before = open_fd_count();
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr = {htonl(INADDR_LOOPBACK)}};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, LISTEN_BACKLOG) == 0);
    socklen_t bytes = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &bytes) == 0);
    struct parent_client_queue queue = {0}; assert(create_parent_client_queue(1, &queue) == 0);
    size_t global_count = 0, accept_calls = 0;
    int peer1 = connect_fixture_client(&address);
    struct pollfd readable = {listener, POLLIN, 0};
    assert(poll(&readable, 1, 1000) == 1);
    assert(parent_accept_has_capacity(&queue, global_count, 2, false));
    int first = accept(listener, NULL, NULL); ++accept_calls; assert(first >= 0); ++global_count;
    struct timespec accepted = {123, 4};
    assert(enqueue_parent_client(&queue, &first, 1, &accepted) == 0);
    int peer2 = connect_fixture_client(&address);
    assert(poll(&readable, 1, 1000) == 1);
    /* The test driver obeys the real helper: no accept while Q is full. */
    if (parent_accept_has_capacity(&queue, global_count, 2, false)) {
        int unexpected = accept(listener, NULL, NULL); ++accept_calls;
        if (unexpected >= 0) close(unexpected);
    }
    assert(accept_calls == 1 && global_count == 1);
    struct parent_queued_client assigned1 = {.client_fd = -1};
    assert(dequeue_parent_client(&queue, &assigned1) == 1);
    assert(parent_accept_has_capacity(&queue, global_count, 2, false));
    int second = accept(listener, NULL, NULL); ++accept_calls; assert(second >= 0); ++global_count;
    assert(enqueue_parent_client(&queue, &second, 2, &accepted) == 0);
    struct parent_queued_client assigned2 = {.client_fd = -1};
    assert(dequeue_parent_client(&queue, &assigned2) == 1);
    assert(queue.count == 0 && !parent_accept_has_capacity(&queue, global_count, 2, false));
    close(assigned1.client_fd); close(assigned2.client_fd); global_count = 0;
    assert(parent_accept_has_capacity(&queue, global_count, 2, false));
    assert(destroy_parent_client_queue(&queue) == 0);
    close(peer1); close(peer2); close(listener);
    assert(open_fd_count() == before);
}

static void test_queue_to_stream_transfer(void) {
    size_t before = open_fd_count();
    struct parent_client_queue queue = {0}; assert(create_parent_client_queue(1, &queue) == 0);
    int client[2], channel[2]; make_client(client); assert(create_worker_fd_channel(channel) == 0);
    struct timespec accepted = {4321, 123};
    assert(enqueue_parent_client(&queue, &client[0], 67, &accepted) == 0);
    struct parent_queued_client selected = {.client_fd = -1};
    assert(dequeue_parent_client(&queue, &selected) == 1);
    struct worker_client_metadata metadata = {1, 7, selected.connection_id, selected.accepted_at};
    struct worker_fd_sender sender = {0}; struct worker_fd_receiver receiver = {0};
    assert(prepare_worker_fd_transfer(&sender, &selected.client_fd, &metadata) == 0 && selected.client_fd == -1);
    struct worker_received_client output = {.client_fd = -1};
    int send_result = WORKER_FD_PENDING, receive_result = WORKER_FD_PENDING;
    for (size_t i = 0; i < 100 && receive_result != WORKER_FD_READY; ++i) {
        if (send_result != WORKER_FD_READY) { send_result = advance_worker_fd_send(channel[0], &sender); assert(send_result >= 0); }
        receive_result = advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output); assert(receive_result >= 0);
    }
    assert(send_result == WORKER_FD_READY && receive_result == WORKER_FD_READY);
    assert(output.metadata.connection_id == 67 && output.metadata.accepted_at.tv_sec == 4321 && output.metadata.accepted_at.tv_nsec == 123);
    assert(!parent_accept_has_capacity(&queue, 1, 1, false)); /* Queue empty != global closed. */
    assert(destroy_parent_client_queue(&queue) == 0);
    assert(send(output.client_fd, "Q", 1, MSG_NOSIGNAL) == 1);
    char marker; assert(recv(client[1], &marker, 1, 0) == 1 && marker == 'Q');
    close(output.client_fd); close(client[1]); close(channel[0]); close(channel[1]);
    assert(open_fd_count() == before);
}

int main(void) {
    alarm(30);
    test_creation_and_invalid_input();
    test_default_capacity_and_wrap();
    test_destroy_pending_and_close_error();
    test_global_and_stopping_gate();
    test_real_listener_pause_and_resume();
    test_queue_to_stream_transfer();
    alarm(0);
    puts("[PASS] parent FIFO: capacity/wrap/ownership, accept gate, loopback pause/resume, STREAM handoff");
    return 0;
}
