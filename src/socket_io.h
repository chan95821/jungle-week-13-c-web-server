#ifndef SOCKET_IO_H
#define SOCKET_IO_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>

/* Existing provisional socket policy, shared without a server/router dependency. */
enum { LISTEN_BACKLOG = 128, IO_CALL_TIMEOUT_SECONDS = 5 };

/* Borrowed cancellation signal: atomic flag plus optional wake-up pipe FD (-1 = none). */
struct cancel_signal {
    const atomic_int *running;
    int stop_fd;
};

/* Borrowed cancellation signal and optional monotonic deadline. */
struct io_context {
    const struct cancel_signal *cancel;
    const struct timespec *deadline;
};
bool cancel_signal_is_running(const struct cancel_signal *cancel);
bool server_is_running(const struct io_context *io);
bool socket_io_timed_out(void);
int parse_port(const char *arg, uint16_t *out_port);
int open_listenfd(uint16_t port);
int set_close_on_exec(int fd);
int socket_configure_timeouts(int fd);
ssize_t receive_with_deadline(int fd, void *buf, size_t len, const struct io_context *io);
int send_allv_with_deadline(int fd, const struct iovec *iov, int iovcnt,
                            const struct io_context *io);
int send_all_with_deadline(int fd, const void *buf, size_t len, const struct io_context *io);
/* Legacy complete-send wrapper without a deadline/cancellation context. */
int send_all(int fd, const void *buf, size_t len);

#endif
