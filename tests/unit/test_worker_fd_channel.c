#define _GNU_SOURCE
#include "../../src/worker_fd_channel.h"

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t send_limit, receive_limit;
static int next_send_error, next_receive_error;
static unsigned int successful_fd_sends, truncated_controls;

ssize_t worker_test_sendmsg(int fd, const struct msghdr *message, int flags) {
    if (next_send_error) { errno = next_send_error; next_send_error = 0; return -1; }
    struct msghdr copy = *message;
    struct iovec vector = message->msg_iov[0];
    if (send_limit && vector.iov_len > send_limit) vector.iov_len = send_limit;
    copy.msg_iov = &vector;
    ssize_t result = sendmsg(fd, &copy, flags);
    if (result > 0 && copy.msg_controllen) ++successful_fd_sends;
    return result;
}

ssize_t worker_test_recvmsg(int fd, struct msghdr *message, int flags) {
    if (next_receive_error) { errno = next_receive_error; next_receive_error = 0; return -1; }
    struct msghdr copy = *message;
    struct iovec vector = message->msg_iov[0];
    if (receive_limit && vector.iov_len > receive_limit) vector.iov_len = receive_limit;
    copy.msg_iov = &vector;
    ssize_t result = recvmsg(fd, &copy, flags);
    message->msg_controllen = copy.msg_controllen;
    message->msg_flags = copy.msg_flags;
    if (result > 0 && (copy.msg_flags & MSG_CTRUNC)) ++truncated_controls;
    return result;
}

static void reset_injection(void) {
    send_limit = receive_limit = 0;
    next_send_error = next_receive_error = 0;
    successful_fd_sends = truncated_controls = 0;
}

static struct worker_client_metadata metadata(uint64_t id) {
    return (struct worker_client_metadata){
        .worker_index = 1, .worker_generation = 7, .connection_id = id,
        .accepted_at = {12345, 987654321}
    };
}

static size_t open_fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++count;
    assert(closedir(directory) == 0);
    return count;
}

static void make_client(int client[2]) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, client) == 0);
}

static void make_frame(unsigned char frame[WORKER_FD_FRAME_BYTES], uint64_t id) {
    int client[2]; make_client(client);
    struct worker_fd_sender sender = {0};
    struct worker_client_metadata info = metadata(id);
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0);
    memcpy(frame, sender.frame, WORKER_FD_FRAME_BYTES);
    abort_worker_fd_send(&sender);
    close(client[1]);
}

static void send_raw_frame(int fd, const void *data, size_t bytes,
                            const int *descriptors, size_t count) {
    assert(count <= 32);
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int) * 32)]; } control = {0};
    struct iovec vector = {(void *)data, bytes};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    if (count) {
        message.msg_control = control.bytes;
        message.msg_controllen = CMSG_SPACE(count * sizeof(int));
        struct cmsghdr *rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET; rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(count * sizeof(int));
        memcpy(CMSG_DATA(rights), descriptors, count * sizeof(int));
    }
    assert(sendmsg(fd, &message, MSG_NOSIGNAL) == (ssize_t)bytes);
}

static void expect_bad_receive(int channel, struct worker_fd_receiver *receiver,
                               uint64_t generation) {
    struct worker_received_client output = {.client_fd = -1};
    int result = WORKER_FD_PENDING;
    for (size_t i = 0; i < 100 && result == WORKER_FD_PENDING; ++i)
        result = advance_worker_fd_receive(channel, 1, generation, receiver, &output);
    assert(result == -1 && output.client_fd == -1);
    assert(receiver->failed && !receiver->owns_client_fd);
    abort_worker_fd_receive(receiver);
}

static void test_channel_flags_and_invalid_prepare(void) {
    int channel[2];
    assert(create_worker_fd_channel(channel) == 0);
    for (size_t i = 0; i < 2; ++i) {
        assert(fcntl(channel[i], F_GETFL) & O_NONBLOCK);
        assert(fcntl(channel[i], F_GETFD) & FD_CLOEXEC);
    }
    struct worker_fd_sender sender = {0};
    int client[2]; make_client(client);
    struct worker_client_metadata info = metadata(1);
    info.accepted_at.tv_nsec = 1000000000L;
    int original = client[0];
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == -1 && errno == EINVAL);
    assert(client[0] == original && !sender.owns_client_fd);
    close(client[0]); close(client[1]);
    int unconnected = socket(AF_UNIX, SOCK_STREAM, 0); assert(unconnected >= 0);
    info = metadata(1);
    assert(prepare_worker_fd_transfer(&sender, &unconnected, &info) == -1 && errno == ENOTCONN);
    assert(!sender.owns_client_fd && unconnected >= 0); close(unconnected);
    close(channel[0]); close(channel[1]);
}

static void test_partial_transfer_and_transient_errors(void) {
    reset_injection();
    size_t before = open_fd_count();
    int channel[2], client[2];
    assert(create_worker_fd_channel(channel) == 0); make_client(client);
    struct worker_fd_sender sender = {0};
    struct worker_fd_receiver receiver = {0};
    struct worker_received_client output = {.client_fd = -1};
    struct worker_client_metadata info = metadata(2);
    int original = client[0];
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0 && client[0] == -1);
    next_send_error = EINTR;
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_PENDING && !sender.fd_sent);
    next_send_error = EAGAIN;
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_PENDING && !sender.fd_sent);
    next_receive_error = EINTR;
    assert(advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) == WORKER_FD_PENDING);
    next_receive_error = EAGAIN;
    assert(advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) == WORKER_FD_PENDING);
    send_limit = receive_limit = 1;
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_PENDING);
    assert(sender.fd_sent && sender.sent_bytes == 1 && fcntl(original, F_GETFD) >= 0);
    next_send_error = EAGAIN;
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_PENDING);
    assert(sender.sent_bytes == 1 && sender.owns_client_fd);
    assert(advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) == WORKER_FD_PENDING);
    assert(receiver.owns_client_fd && output.client_fd == -1);
    int send_result = WORKER_FD_PENDING, receive_result = WORKER_FD_PENDING;
    for (size_t i = 0; i < 100 && receive_result != WORKER_FD_READY; ++i) {
        if (send_result != WORKER_FD_READY) {
            send_result = advance_worker_fd_send(channel[0], &sender);
            assert(send_result >= 0);
            if (send_result == WORKER_FD_READY) assert(fcntl(original, F_GETFD) == -1 && errno == EBADF);
        }
        receive_result = advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output);
        assert(receive_result >= 0);
        if (receive_result != WORKER_FD_READY) assert(output.client_fd == -1);
    }
    assert(send_result == WORKER_FD_READY && receive_result == WORKER_FD_READY);
    assert(successful_fd_sends == 1 && !sender.owns_client_fd && !receiver.owns_client_fd);
    assert(output.metadata.connection_id == info.connection_id);
    assert(output.metadata.accepted_at.tv_sec == info.accepted_at.tv_sec);
    assert(output.metadata.accepted_at.tv_nsec == info.accepted_at.tv_nsec);
    assert(fcntl(output.client_fd, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(output.client_fd, F_GETFL) & O_NONBLOCK);
    assert(send(output.client_fd, "OK", 2, MSG_NOSIGNAL) == 2);
    char answer[2]; assert(recv(client[1], answer, sizeof(answer), 0) == 2 && !memcmp(answer, "OK", 2));
    close(output.client_fd); close(client[1]); close(channel[0]); close(channel[1]);
    assert(open_fd_count() == before);
    reset_injection();
}

static void test_real_channel_eagain(void) {
    reset_injection(); size_t before = open_fd_count();
    int channel[2], client[2]; assert(create_worker_fd_channel(channel) == 0); make_client(client);
    int buffer_size = 4096;
    assert(setsockopt(channel[0], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == 0);
    unsigned char filler[1024] = {0};
    bool blocked = false;
    for (size_t i = 0; i < 256; ++i) {
        ssize_t count = send(channel[0], filler, sizeof(filler), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0) { assert(errno == EAGAIN); blocked = true; break; }
        assert(count > 0);
    }
    assert(blocked);
    struct worker_fd_sender sender = {0}; struct worker_client_metadata info = metadata(200);
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0);
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_PENDING);
    assert(!sender.fd_sent && sender.sent_bytes == 0 && sender.owns_client_fd);
    ssize_t drained;
    do { drained = recv(channel[1], filler, sizeof(filler), MSG_DONTWAIT); } while (drained > 0);
    assert(drained == -1 && errno == EAGAIN);
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_READY);
    struct worker_fd_receiver receiver = {0}; struct worker_received_client output = {.client_fd = -1};
    int result;
    do { result = advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output); } while (result == WORKER_FD_PENDING);
    assert(result == WORKER_FD_READY && successful_fd_sends == 1);
    close(output.client_fd); close(client[1]); close(channel[0]); close(channel[1]);
    assert(open_fd_count() == before);
}
static void test_two_coalesced_frames(void) {
    reset_injection();
    size_t before = open_fd_count();
    int channel[2], client[2][2]; assert(create_worker_fd_channel(channel) == 0);
    for (size_t i = 0; i < 2; ++i) {
        make_client(client[i]);
        struct worker_fd_sender sender = {0};
        struct worker_client_metadata info = metadata(10 + i);
        assert(prepare_worker_fd_transfer(&sender, &client[i][0], &info) == 0);
        assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_READY);
    }
    struct worker_fd_receiver receiver = {0};
    for (size_t i = 0; i < 2; ++i) {
        struct worker_received_client output = {.client_fd = -1};
        int result;
        do { result = advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output); } while (result == WORKER_FD_PENDING);
        assert(result == WORKER_FD_READY && output.metadata.connection_id == 10 + i);
        char marker = (char)('A' + i), echoed;
        assert(send(output.client_fd, &marker, 1, MSG_NOSIGNAL) == 1);
        assert(recv(client[i][1], &echoed, 1, 0) == 1 && echoed == marker);
        close(output.client_fd); close(client[i][1]);
    }
    assert(successful_fd_sends == 2);
    close(channel[0]);
    struct worker_received_client output = {.client_fd = -1};
    assert(advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) == WORKER_FD_EOF);
    close(channel[1]);
    assert(open_fd_count() == before);
}

static void test_malformed_frames_and_fd_counts(void) {
    for (unsigned int kind = 0; kind < 10; ++kind) {
        size_t before = open_fd_count();
        int channel[2], client[2]; assert(create_worker_fd_channel(channel) == 0); make_client(client);
        unsigned char frame[WORKER_FD_FRAME_BYTES]; make_frame(frame, 40);
        if (kind == 0) frame[0] ^= 1;
        if (kind == 1) frame[7] = 2;
        if (kind == 2) frame[11] = 2;
        if (kind == 3) frame[47] = 1;
        if (kind == 4) memset(frame + 16, 0, 8);
        if (kind == 5) memset(frame + 24, 0, 8);
        if (kind == 6) memset(frame + 40, 255, 4);
        if (kind == 7) frame[32] = 128;
        if (kind == 8) frame[15] = 9;
        send_raw_frame(channel[0], frame, sizeof(frame), &client[0], 1);
        close(client[0]);
        struct worker_fd_receiver receiver = {0};
        expect_bad_receive(channel[1], &receiver, kind == 9 ? 8 : 7);
        close(client[1]); close(channel[0]); close(channel[1]);
        assert(open_fd_count() == before);
    }
    const size_t fd_counts[] = {0, 2, 32};
    for (size_t kind = 0; kind < 3; ++kind) {
        size_t before = open_fd_count();
        int channel[2], client[2]; assert(create_worker_fd_channel(channel) == 0); make_client(client);
        unsigned char frame[WORKER_FD_FRAME_BYTES]; make_frame(frame, 50);
        int descriptors[32];
        for (size_t i = 0; i < 32; ++i) descriptors[i] = client[0];
        unsigned int truncation_before = truncated_controls;
        send_raw_frame(channel[0], frame, sizeof(frame), descriptors, fd_counts[kind]);
        close(client[0]);
        struct worker_fd_receiver receiver = {0};
        expect_bad_receive(channel[1], &receiver, 7);
        if (fd_counts[kind] == 32) assert(truncated_controls > truncation_before);
        close(client[1]); close(channel[0]); close(channel[1]);
        assert(open_fd_count() == before);
    }
}

static void test_late_ancillary_and_regular_file(void) {
    size_t before = open_fd_count();
    int channel[2], client[2]; assert(create_worker_fd_channel(channel) == 0); make_client(client);
    unsigned char frame[WORKER_FD_FRAME_BYTES]; make_frame(frame, 60);
    send_raw_frame(channel[0], frame, 1, &client[0], 1);
    struct worker_fd_receiver receiver = {0};
    struct worker_received_client output = {.client_fd = -1};
    assert(advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) == WORKER_FD_PENDING);
    assert(receiver.owns_client_fd);
    send_raw_frame(channel[0], frame + 1, sizeof(frame) - 1, &client[0], 1);
    expect_bad_receive(channel[1], &receiver, 7);
    close(client[0]); close(client[1]); close(channel[0]); close(channel[1]);
    assert(open_fd_count() == before);

    assert(create_worker_fd_channel(channel) == 0);
    int file = open("/dev/null", O_RDONLY); assert(file >= 0);
    send_raw_frame(channel[0], frame, sizeof(frame), &file, 1); close(file);
    receiver = (struct worker_fd_receiver){0};
    expect_bad_receive(channel[1], &receiver, 7);
    close(channel[0]); close(channel[1]); assert(open_fd_count() == before);
}

static void test_partial_eof_cancel_and_peer_close(void) {
    reset_injection();
    size_t before = open_fd_count();
    int channel[2], client[2]; assert(create_worker_fd_channel(channel) == 0); make_client(client);
    struct worker_fd_sender sender = {0}; struct worker_fd_receiver receiver = {0};
    struct worker_received_client output = {.client_fd = -1};
    struct worker_client_metadata info = metadata(70);
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0);
    send_limit = 1;
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_PENDING);
    assert(advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) == WORKER_FD_PENDING);
    abort_worker_fd_send(&sender); abort_worker_fd_send(&sender); close(channel[0]);
    assert(advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) == -1 && errno == ECONNRESET);
    assert(!receiver.owns_client_fd);
    int reused = dup(STDOUT_FILENO); assert(reused >= 0);
    abort_worker_fd_receive(&receiver); abort_worker_fd_receive(&receiver);
    assert(fcntl(reused, F_GETFD) >= 0); close(reused);
    char byte; assert(recv(client[1], &byte, 1, MSG_DONTWAIT) == 0);
    close(client[1]); close(channel[1]); assert(open_fd_count() == before);

    reset_injection();
    assert(create_worker_fd_channel(channel) == 0); make_client(client);
    sender = (struct worker_fd_sender){0};
    int original = client[0];
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0);
    close(channel[1]);
    assert(advance_worker_fd_send(channel[0], &sender) == -1);
    assert(sender.failed && !sender.owns_client_fd && !sender.fd_sent);
    assert(fcntl(original, F_GETFD) == -1 && errno == EBADF);
    abort_worker_fd_send(&sender); close(client[1]); close(channel[0]);
    assert(open_fd_count() == before);
}

static int await_received_client(int channel, struct worker_received_client *output) {
    struct worker_fd_receiver receiver = {0};
    for (size_t i = 0; i < 100; ++i) {
        int result = advance_worker_fd_receive(channel, 1, 7, &receiver, output);
        if (result != WORKER_FD_PENDING) return result;
        struct pollfd event = {channel, POLLIN, 0};
        poll(&event, 1, 10); /* Test readiness wait, not a product timeout. */
    }
    abort_worker_fd_receive(&receiver);
    return -1;
}

static void test_fork_transfer_including_fd_zero(void) {
    reset_injection(); size_t before = open_fd_count();
    int channel[2], client[2]; assert(create_worker_fd_channel(channel) == 0); make_client(client);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        close(channel[0]); close(client[0]); close(client[1]); close(STDIN_FILENO);
        struct worker_received_client output = {.client_fd = -1};
        if (await_received_client(channel[1], &output) != WORKER_FD_READY || output.client_fd != 0) _exit(2);
        if (!(fcntl(output.client_fd, F_GETFD) & FD_CLOEXEC) ||
            !(fcntl(output.client_fd, F_GETFL) & O_NONBLOCK)) _exit(3);
        if (send(output.client_fd, "OK", 2, MSG_NOSIGNAL) != 2) _exit(4);
        close(output.client_fd); close(channel[1]); _exit(0);
    }
    close(channel[1]);
    struct worker_fd_sender sender = {0}; struct worker_client_metadata info = metadata(80);
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0);
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_READY);
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char answer[2]; assert(recv(client[1], answer, 2, 0) == 2 && !memcmp(answer, "OK", 2));
    close(client[1]); close(channel[0]); assert(open_fd_count() == before);
}

static void test_worker_exit_with_queued_fd(void) {
    reset_injection(); size_t before = open_fd_count();
    int channel[2], client[2], ready[2], leave[2];
    assert(create_worker_fd_channel(channel) == 0); make_client(client);
    assert(pipe(ready) == 0 && pipe(leave) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        close(channel[0]); close(client[0]); close(client[1]); close(ready[0]); close(leave[1]);
        if (write(ready[1], "R", 1) != 1) _exit(2);
        char marker; if (read(leave[0], &marker, 1) != 1) _exit(3);
        /* Deliberately never recvmsg; exit must release the kernel queued FD. */
        _exit(0);
    }
    close(channel[1]); close(ready[1]); close(leave[0]);
    char marker; assert(read(ready[0], &marker, 1) == 1); close(ready[0]);
    struct worker_fd_sender sender = {0}; struct worker_client_metadata info = metadata(90);
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0);
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_READY);
    assert(recv(client[1], &marker, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    assert(kill(child, SIGKILL) == 0); close(leave[1]);
    int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(recv(client[1], &marker, 1, MSG_DONTWAIT) == 0);
    close(client[1]); close(channel[0]); assert(open_fd_count() == before);
}

static void test_worker_exit_after_receiving_anchor(void) {
    reset_injection(); size_t before = open_fd_count();
    int channel[2], client[2], ready[2], leave[2];
    assert(create_worker_fd_channel(channel) == 0); make_client(client);
    assert(pipe(ready) == 0 && pipe(leave) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        close(channel[0]); close(client[0]); close(client[1]); close(ready[0]); close(leave[1]);
        struct worker_fd_receiver receiver = {0}; struct worker_received_client output = {.client_fd = -1};
        for (size_t i = 0; i < 100 && !receiver.owns_client_fd; ++i) {
            if (advance_worker_fd_receive(channel[1], 1, 7, &receiver, &output) < 0) _exit(2);
            struct pollfd event = {channel[1], POLLIN, 0}; poll(&event, 1, 10);
        }
        if (!receiver.owns_client_fd || output.client_fd != -1) _exit(3);
        if (write(ready[1], "R", 1) != 1) _exit(4);
        char marker; if (read(leave[0], &marker, 1) != 1) _exit(5);
        _exit(0); /* No abort helper: process death releases the held FD. */
    }
    close(channel[1]); close(ready[1]); close(leave[0]);
    struct worker_fd_sender sender = {0}; struct worker_client_metadata info = metadata(100);
    assert(prepare_worker_fd_transfer(&sender, &client[0], &info) == 0);
    send_limit = 1;
    assert(advance_worker_fd_send(channel[0], &sender) == WORKER_FD_PENDING && sender.fd_sent);
    char marker; assert(read(ready[0], &marker, 1) == 1); close(ready[0]);
    assert(sender.owns_client_fd);
    assert(kill(child, SIGKILL) == 0); close(leave[1]);
    int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(advance_worker_fd_send(channel[0], &sender) == -1);
    assert(sender.failed && sender.fd_sent && !sender.owns_client_fd);
    assert(recv(client[1], &marker, 1, MSG_DONTWAIT) == 0);
    close(client[1]); close(channel[0]); assert(open_fd_count() == before);
    reset_injection();
}

static void test_tcp_reset_before_fd_delivery(void) {
    reset_injection();size_t before=open_fd_count();int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    assert(bind(listener,(struct sockaddr *)&address,sizeof(address))==0 && listen(listener,1)==0);
    socklen_t length=sizeof(address);assert(getsockname(listener,(struct sockaddr *)&address,&length)==0);
    int peer=socket(AF_INET,SOCK_STREAM,0);assert(peer>=0 && connect(peer,(struct sockaddr *)&address,sizeof(address))==0);
    int accepted=accept(listener,NULL,NULL);assert(accepted>=0);close(listener);int monitor=dup(accepted);assert(monitor>=0);
    int channel[2];assert(create_worker_fd_channel(channel)==0);
    struct worker_fd_sender sender={0};struct worker_client_metadata info=metadata(101);
    assert(prepare_worker_fd_transfer(&sender,&accepted,&info)==0 && advance_worker_fd_send(channel[0],&sender)==WORKER_FD_READY);
    struct linger reset={1,0};assert(setsockopt(peer,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset))==0);close(peer);
    struct pollfd ready={.fd=monitor,.events=POLLERR};assert(poll(&ready,1,1000)>0 && (ready.revents&POLLERR));
    struct sockaddr_storage remote;length=sizeof(remote);assert(getpeername(monitor,(struct sockaddr *)&remote,&length)==-1 && errno==ENOTCONN);close(monitor);
    struct worker_fd_receiver receiver={0};struct worker_received_client received={.client_fd=-1};int result;
    do{result=advance_worker_fd_receive(channel[1],1,7,&receiver,&received);}while(result==WORKER_FD_PENDING);
    assert(result==WORKER_FD_CLIENT_GONE && received.client_fd==-1 && received.metadata.connection_id==101 && !receiver.failed);
    /* Valid cancellation must not poison the channel's next real FD frame. */
    int client[2];make_client(client);sender=(struct worker_fd_sender){0};info=metadata(102);
    assert(prepare_worker_fd_transfer(&sender,&client[0],&info)==0 && advance_worker_fd_send(channel[0],&sender)==WORKER_FD_READY);
    do{result=advance_worker_fd_receive(channel[1],1,7,&receiver,&received);}while(result==WORKER_FD_PENDING);
    assert(result==WORKER_FD_READY && received.client_fd>=0 && received.metadata.connection_id==102);
    close(received.client_fd);close(client[1]);close(channel[0]);close(channel[1]);assert(open_fd_count()==before);
}
int main(void) {
    alarm(30);
    test_channel_flags_and_invalid_prepare();
    test_partial_transfer_and_transient_errors();
    puts("[PASS] partial send/receive, EINTR/EAGAIN, one SCM_RIGHTS, source lifetime");
    test_real_channel_eagain();
    puts("[PASS] actual socket-buffer EAGAIN and resume");
    test_two_coalesced_frames();
    puts("[PASS] two coalesced frames retain their own FD and metadata");
    test_malformed_frames_and_fd_counts();
    test_late_ancillary_and_regular_file();
    puts("[PASS] malformed frame/ancillary including observed MSG_CTRUNC: FD counts restored");
    test_partial_eof_cancel_and_peer_close();
    test_fork_transfer_including_fd_zero();
    test_worker_exit_with_queued_fd();
    test_worker_exit_after_receiving_anchor();
    puts("[PASS] fork transfer with received FD 0; SIGKILL before recv and after anchor; peer EOF");
    test_tcp_reset_before_fd_delivery();
    puts("[PASS] actual TCP reset before FD receive: client cancellation, next frame intact, FD baseline restored");
    alarm(0);
    puts("[PASS] STREAM FD frames: partial I/O, ancillary once, malformed cleanup, fork/exit, FD counts");
    return 0;
}
