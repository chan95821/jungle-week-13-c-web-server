#define _GNU_SOURCE
#include "worker_closed_channel.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLOSED_MAGIC UINT32_C(0x4a57434c)
#ifdef WORKER_CLOSED_CHANNEL_TEST
extern ssize_t closed_test_send(int fd, const void *buffer, size_t length, int flags);
extern ssize_t closed_test_recvmsg(int fd, struct msghdr *message, int flags);
#define closed_send closed_test_send
#define closed_recvmsg closed_test_recvmsg
#else
#define closed_send send
#define closed_recvmsg recvmsg
#endif
static int invalid(int error) { errno=error; return -1; }
static void put32(unsigned char *p,uint32_t v) {
    for (unsigned int i=0;i<4;++i) p[i]=(unsigned char)(v>>(24-8*i));
}
static uint32_t get32(const unsigned char *p) {
    uint32_t v=0; for (unsigned int i=0;i<4;++i) v=(v<<8)|p[i]; return v;
}
static void put64(unsigned char *p,uint64_t v) { put32(p,(uint32_t)(v>>32)); put32(p+4,(uint32_t)v); }
static uint64_t get64(const unsigned char *p) { return ((uint64_t)get32(p)<<32)|get32(p+4); }
static bool valid_closed(const struct worker_connection_closed *closed) {
    return closed && closed->worker_generation && (uint32_t)closed->connection_id &&
           (closed->connection_id>>32) && valid_distribution_observation(&closed->observation);
}
static bool valid_queue(const struct worker_closed_queue *queue) {
    return queue && queue->items && queue->capacity && queue->head<queue->capacity &&
           queue->count<=queue->capacity && queue->sent_bytes<WORKER_CLOSED_FRAME_BYTES;
}
int create_worker_closed_queue(size_t capacity,struct worker_closed_queue *queue) {
    if (!queue || !capacity) return invalid(EINVAL);
    if (queue->items) return invalid(EBUSY);
    if (capacity>UINT32_MAX || capacity>SIZE_MAX/sizeof(*queue->items)) return invalid(EOVERFLOW);
    struct worker_connection_closed *items=calloc(capacity,sizeof(*items));
    if (!items) return -1;
    *queue=(struct worker_closed_queue){.items=items,.capacity=capacity};
    return 0;
}
void destroy_worker_closed_queue(struct worker_closed_queue *queue) {
    if (!queue) return;
    free(queue->items); *queue=(struct worker_closed_queue){0};
}
bool worker_closed_has_capacity(const struct worker_closed_queue *queue) {
    return valid_queue(queue) && !queue->failed && queue->count<queue->capacity;
}
int enqueue_worker_connection_closed(struct worker_closed_queue *queue,
                                      const struct worker_connection_closed *closed) {
    if (!valid_queue(queue) || !valid_closed(closed)) return invalid(EINVAL);
    if (queue->failed) return invalid(EPROTO);
    for (size_t i=0;i<queue->count;++i) {
        const struct worker_connection_closed *item=&queue->items[(queue->head+i)%queue->capacity];
        if (item->worker_index==closed->worker_index && item->worker_generation==closed->worker_generation &&
            item->connection_id==closed->connection_id) return 1;
    }
    if (queue->count==queue->capacity) return invalid(ENOSPC);
    queue->items[(queue->head+queue->count)%queue->capacity]=*closed;
    ++queue->count; return 0;
}
int advance_worker_closed_send(int fd,struct worker_closed_queue *queue) {
    if (!valid_queue(queue)) return invalid(EINVAL);
    if (queue->failed) return invalid(EPROTO);
    if (!queue->count) return WORKER_CLOSED_PENDING;
    if (!queue->sent_bytes) {
        const struct worker_connection_closed *item=&queue->items[queue->head];
        put32(queue->frame,CLOSED_MAGIC); put32(queue->frame+4,2);
        put32(queue->frame+8,item->worker_index); put32(queue->frame+12,0);
        put64(queue->frame+16,item->worker_generation); put64(queue->frame+24,item->connection_id);
        put64(queue->frame+32,item->observation.elapsed_us);
        put64(queue->frame+40,item->observation.finished_us);
        put32(queue->frame+48,(uint32_t)item->observation.result);put32(queue->frame+52,0);
    }
    size_t remaining=WORKER_CLOSED_FRAME_BYTES-queue->sent_bytes;
    ssize_t sent=closed_send(fd,queue->frame+queue->sent_bytes,remaining,MSG_DONTWAIT|MSG_NOSIGNAL);
    if (sent<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) return WORKER_CLOSED_PENDING;
    if (sent<=0 || (size_t)sent>remaining) {
        int error=sent<0?errno:(sent==0?EPIPE:EPROTO); queue->failed=true; return invalid(error);
    }
    queue->sent_bytes+=(size_t)sent;
    if (queue->sent_bytes!=WORKER_CLOSED_FRAME_BYTES) return WORKER_CLOSED_PENDING;
    queue->items[queue->head]=(struct worker_connection_closed){0};
    queue->head=(queue->head+1)%queue->capacity; --queue->count; queue->sent_bytes=0;
    return WORKER_CLOSED_READY;
}
int advance_worker_closed_receive(int fd,uint32_t worker,uint64_t generation,
                                   struct worker_closed_receiver *receiver,
                                   struct worker_connection_closed *closed) {
    if (!receiver || !closed || !generation || receiver->received_bytes>=WORKER_CLOSED_FRAME_BYTES)
        return invalid(EINVAL);
    if (receiver->failed) return invalid(EPROTO);
    struct iovec vector={receiver->frame+receiver->received_bytes,WORKER_CLOSED_FRAME_BYTES-receiver->received_bytes};
    union { struct cmsghdr alignment; unsigned char bytes[CMSG_SPACE(16*sizeof(int))]; } control={0};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,
                           .msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t received=closed_recvmsg(fd,&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if (received<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) return WORKER_CLOSED_PENDING;
    bool ancillary=(message.msg_flags&MSG_CTRUNC)!=0;
    if (received>=0) {
        for (struct cmsghdr *header=CMSG_FIRSTHDR(&message);header;header=CMSG_NXTHDR(&message,header)) {
            ancillary=true;
            if (header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS && header->cmsg_len>=CMSG_LEN(0)) {
                size_t count=(header->cmsg_len-CMSG_LEN(0))/sizeof(int);
                for (size_t i=0;i<count;++i) {
                    int unexpected; memcpy(&unexpected,(unsigned char *)CMSG_DATA(header)+i*sizeof(int),sizeof(int));
                    close(unexpected); /* Linux: never retry close(EINTR). */
                }
            }
        }
    }
    if (received<0 || ancillary || (size_t)received>vector.iov_len) {
        int error=received<0?errno:EPROTO; receiver->failed=true; return invalid(error);
    }
    if (!received) {
        receiver->failed=true;
        return receiver->received_bytes?invalid(EPROTO):WORKER_CLOSED_EOF;
    }
    receiver->received_bytes+=(size_t)received;
    if (receiver->received_bytes!=WORKER_CLOSED_FRAME_BYTES) return WORKER_CLOSED_PENDING;
    const unsigned char *frame=receiver->frame;
    struct worker_connection_closed result={.worker_index=get32(frame+8),
        .worker_generation=get64(frame+16),.connection_id=get64(frame+24),
        .observation={get64(frame+32),get64(frame+40),(enum distribution_sample_result)get32(frame+48)}};
    if (get32(frame)!=CLOSED_MAGIC || get32(frame+4)!=2 || get32(frame+12)!=0 || get32(frame+52)!=0 ||
        result.worker_index!=worker || result.worker_generation!=generation || !valid_closed(&result)) {
        receiver->failed=true; return invalid(EPROTO);
    }
    *closed=result; receiver->received_bytes=0;
    return WORKER_CLOSED_READY;
}
