#define _GNU_SOURCE
#include "worker_fd_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WORKER_FD_MAGIC UINT32_C(0x4a574644)
enum { WORKER_FD_VERSION = 1, WORKER_FD_CLIENT_TYPE = 1, RECEIVED_FD_CHECK_LIMIT = 8 };

#ifdef WORKER_FD_CHANNEL_TEST
extern ssize_t worker_test_sendmsg(int fd, const struct msghdr *message, int flags);
extern ssize_t worker_test_recvmsg(int fd, struct msghdr *message, int flags);
#define channel_sendmsg worker_test_sendmsg
#define channel_recvmsg worker_test_recvmsg
#else
#define channel_sendmsg sendmsg
#define channel_recvmsg recvmsg
#endif

static int fail_sender(struct worker_fd_sender *sender, int error);
static int fail_receiver(struct worker_fd_receiver *receiver, int error);
static int extract_received_fd(const struct msghdr *message, bool first_byte, int *received_fd);
static int make_client_nonblocking(int fd);
static int validate_stream_socket(int fd);
static int validate_metadata(const struct worker_client_metadata *metadata);
static int decode_client_frame(const unsigned char *frame, struct worker_client_metadata *metadata);
static void encode_client_frame(unsigned char *frame, const struct worker_client_metadata *metadata);
static void put_u32(unsigned char *bytes, uint32_t value);
static void put_u64(unsigned char *bytes, uint64_t value);
static uint32_t get_u32(const unsigned char *bytes);
static uint64_t get_u64(const unsigned char *bytes);

int create_worker_fd_channel(int channel[2]) {
    if (!channel) { errno = EINVAL; return -1; }
    channel[0] = channel[1] = -1;
    return socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, channel);
}

int prepare_worker_fd_transfer(struct worker_fd_sender *sender, int *client_fd,
                               const struct worker_client_metadata *metadata) {
    if (!sender || !client_fd || *client_fd < 0 || !metadata) { errno = EINVAL; return -1; }
    if (sender->owns_client_fd) { errno = EBUSY; return -1; }
    if (sender->failed) { errno = EPROTO; return -1; }
    if (validate_metadata(metadata) < 0 || validate_stream_socket(*client_fd) < 0) return -1;
    *sender = (struct worker_fd_sender){.client_fd = *client_fd, .owns_client_fd = true};
    encode_client_frame(sender->frame, metadata);
    *client_fd = -1;
    return 0;
}

int advance_worker_fd_send(int channel_fd, struct worker_fd_sender *sender) {
    if (!sender) { errno = EINVAL; return -1; }
    if (sender->failed) { errno = EPROTO; return -1; }
    if (sender->finished) return WORKER_FD_READY;
    if (!sender->owns_client_fd || sender->sent_bytes >= WORKER_FD_FRAME_BYTES)
        return fail_sender(sender, EINVAL);

    struct iovec vector = {sender->frame + sender->sent_bytes,
                           WORKER_FD_FRAME_BYTES - sender->sent_bytes};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};
    union { struct cmsghdr alignment; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control = {0};
    if (!sender->fd_sent) {
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *rights = CMSG_FIRSTHDR(&message);
        rights->cmsg_level = SOL_SOCKET;
        rights->cmsg_type = SCM_RIGHTS;
        rights->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(rights), &sender->client_fd, sizeof(int));
    }
    ssize_t count = channel_sendmsg(channel_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return WORKER_FD_PENDING;
        return fail_sender(sender, errno);
    }
    if (!count) return fail_sender(sender, EPIPE);
    sender->fd_sent = true;
    if ((size_t)count > vector.iov_len) return fail_sender(sender, EPROTO);
    sender->sent_bytes += (size_t)count;
    if (sender->sent_bytes != WORKER_FD_FRAME_BYTES) return WORKER_FD_PENDING;

    int original = sender->client_fd;
    sender->owns_client_fd = false;
    sender->client_fd = -1;
    sender->finished = true;
    /* Linux: never retry close after EINTR, including a completed transfer. */
    if (close(original) < 0) return fail_sender(sender, errno);
    return WORKER_FD_READY;
}

int advance_worker_fd_receive(int channel_fd, uint32_t expected_worker,
                              uint64_t expected_generation,
                              struct worker_fd_receiver *receiver,
                              struct worker_received_client *received) {
    if (!receiver || !received || !expected_generation) { errno = EINVAL; return -1; }
    if (received->client_fd >= 0) { errno = EBUSY; return -1; }
    if (receiver->failed) { errno = EPROTO; return -1; }
    if (receiver->received_bytes >= WORKER_FD_FRAME_BYTES)
        return fail_receiver(receiver, EPROTO);
    bool first_byte = receiver->received_bytes == 0;
    size_t bytes_to_read = first_byte ? 1 : WORKER_FD_FRAME_BYTES - receiver->received_bytes;
    struct iovec vector = {receiver->frame + receiver->received_bytes, bytes_to_read};
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int) * RECEIVED_FD_CHECK_LIMIT)];
    } control = {0};
    struct msghdr message = {
        .msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)
    };
    ssize_t count = channel_recvmsg(channel_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return WORKER_FD_PENDING;
        return fail_receiver(receiver, errno);
    }
    if (!count) {
        if (first_byte && !receiver->owns_client_fd) return WORKER_FD_EOF;
        return fail_receiver(receiver, ECONNRESET);
    }
    int incoming = -1;
    if (extract_received_fd(&message, first_byte, &incoming) < 0)
        return fail_receiver(receiver, errno);
    if (incoming >= 0) { receiver->client_fd = incoming; receiver->owns_client_fd = true; }
    if ((size_t)count > bytes_to_read) return fail_receiver(receiver, EPROTO);
    receiver->received_bytes += (size_t)count;
    if (receiver->received_bytes != WORKER_FD_FRAME_BYTES) return WORKER_FD_PENDING;

    struct worker_client_metadata metadata;
    if (decode_client_frame(receiver->frame, &metadata) < 0)
        return fail_receiver(receiver, errno);
    if (metadata.worker_index != expected_worker || metadata.worker_generation != expected_generation)
        return fail_receiver(receiver, ESTALE);
    if (!receiver->owns_client_fd) return fail_receiver(receiver, EPROTO);
    if (make_client_nonblocking(receiver->client_fd) < 0) {
        int validation_error=errno, socket_error=0;
        socklen_t error_size=sizeof(socket_error);
        /* getpeername may become ENOTCONN after an accepted TCP socket resets
         * in transit. Require reset evidence; an unconnected socket (error=0)
         * or malformed frame is still a control-channel failure. */
        if(validation_error==ENOTCONN && getsockopt(receiver->client_fd,SOL_SOCKET,SO_ERROR,
            &socket_error,&error_size)==0 && (socket_error==ECONNRESET || socket_error==ECONNABORTED)) {
            close(receiver->client_fd);
            *received=(struct worker_received_client){.client_fd=-1,.metadata=metadata};
            *receiver=(struct worker_fd_receiver){0};return WORKER_FD_CLIENT_GONE;
        }
        return fail_receiver(receiver,validation_error);
    }
    *received = (struct worker_received_client){receiver->client_fd, metadata};
    *receiver = (struct worker_fd_receiver){0};
    return WORKER_FD_READY;
}

void abort_worker_fd_send(struct worker_fd_sender *sender) {
    if (!sender) return;
    if (sender->owns_client_fd) {
        int fd = sender->client_fd;
        sender->owns_client_fd = false;
        sender->client_fd = -1;
        close(fd);
    }
    sender->failed = true;
}

void abort_worker_fd_receive(struct worker_fd_receiver *receiver) {
    if (!receiver) return;
    if (receiver->owns_client_fd) {
        int fd = receiver->client_fd;
        receiver->owns_client_fd = false;
        receiver->client_fd = -1;
        close(fd);
    }
    receiver->failed = true;
}

static int fail_sender(struct worker_fd_sender *sender, int error) {
    abort_worker_fd_send(sender);
    errno = error;
    return -1;
}

static int fail_receiver(struct worker_fd_receiver *receiver, int error) {
    abort_worker_fd_receive(receiver);
    errno = error;
    return -1;
}

static int extract_received_fd(const struct msghdr *message, bool first_byte, int *received_fd) {
    int first = -1;
    size_t count = 0;
    bool invalid = (message->msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0;
    for (struct cmsghdr *control = CMSG_FIRSTHDR(message); control;
         control = CMSG_NXTHDR((struct msghdr *)message, control)) {
        size_t offset = (size_t)((unsigned char *)control - (unsigned char *)message->msg_control);
        if (control->cmsg_len < CMSG_LEN(0) || offset > message->msg_controllen ||
            control->cmsg_len > message->msg_controllen - offset) { invalid = true; break; }
        if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS) {
            invalid = true;
            continue;
        }
        size_t bytes = control->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) invalid = true;
        for (size_t i = 0; i < bytes / sizeof(int); ++i) {
            int fd;
            memcpy(&fd, CMSG_DATA(control) + i * sizeof(int), sizeof(fd));
            if (count++ == 0) first = fd;
            else { close(fd); invalid = true; }
        }
    }
    if ((first_byte && count != 1) || (!first_byte && count != 0)) invalid = true;
    if (invalid) {
        if (first >= 0) close(first);
        errno = EPROTO;
        return -1;
    }
    *received_fd = first;
    return 0;
}

static int validate_stream_socket(int fd) {
    int type;
    socklen_t bytes = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &bytes) < 0) return -1;
    if (type != SOCK_STREAM) { errno = EPROTOTYPE; return -1; }
    struct sockaddr_storage peer;
    bytes = sizeof(peer);
    return getpeername(fd, (struct sockaddr *)&peer, &bytes);
}

static int make_client_nonblocking(int fd) {
    if (validate_stream_socket(fd) < 0) return -1;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    if (!(flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static int validate_metadata(const struct worker_client_metadata *metadata) {
    if (!metadata->worker_generation || !metadata->connection_id ||
        metadata->accepted_at.tv_sec < 0 || metadata->accepted_at.tv_nsec < 0 ||
        metadata->accepted_at.tv_nsec >= 1000000000L ||
        (uintmax_t)metadata->accepted_at.tv_sec > INT64_MAX) { errno = EINVAL; return -1; }
    return 0;
}

static void encode_client_frame(unsigned char *frame, const struct worker_client_metadata *metadata) {
    put_u32(frame, WORKER_FD_MAGIC);
    put_u32(frame + 4, WORKER_FD_VERSION);
    put_u32(frame + 8, WORKER_FD_CLIENT_TYPE);
    put_u32(frame + 12, metadata->worker_index);
    put_u64(frame + 16, metadata->worker_generation);
    put_u64(frame + 24, metadata->connection_id);
    put_u64(frame + 32, (uint64_t)metadata->accepted_at.tv_sec);
    put_u32(frame + 40, (uint32_t)metadata->accepted_at.tv_nsec);
    put_u32(frame + 44, 0);
}

static int decode_client_frame(const unsigned char *frame, struct worker_client_metadata *metadata) {
    uint64_t seconds = get_u64(frame + 32);
    if (get_u32(frame) != WORKER_FD_MAGIC || get_u32(frame + 4) != WORKER_FD_VERSION ||
        get_u32(frame + 8) != WORKER_FD_CLIENT_TYPE || get_u32(frame + 44) || seconds > INT64_MAX) {
        errno = EPROTO;
        return -1;
    }
    *metadata = (struct worker_client_metadata){
        .worker_index = get_u32(frame + 12), .worker_generation = get_u64(frame + 16),
        .connection_id = get_u64(frame + 24),
        .accepted_at = {(time_t)seconds, (long)get_u32(frame + 40)}
    };
    if ((uint64_t)metadata->accepted_at.tv_sec != seconds || validate_metadata(metadata) < 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static void put_u32(unsigned char *bytes, uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (24 - 8 * i));
}
static void put_u64(unsigned char *bytes, uint64_t value) {
    for (unsigned int i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (56 - 8 * i));
}
static uint32_t get_u32(const unsigned char *bytes) {
    uint32_t value = 0;
    for (unsigned int i = 0; i < 4; ++i) value = (value << 8) | bytes[i];
    return value;
}
static uint64_t get_u64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}
