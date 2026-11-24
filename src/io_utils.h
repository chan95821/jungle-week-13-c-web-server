#ifndef IO_UTILS_H
#define IO_UTILS_H

#include <stdbool.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>

enum { IO_CALL_TIMEOUT_SECONDS = 5 };

/* 소유권을 가지지 않는 기존 서버 I/O 제어 정보. NULL 포인터는 해당 제한이 없음을 뜻한다. */
struct io_context {
    const volatile sig_atomic_t *running;
    const struct timespec *deadline;
};

/* 종료 요청이 없으면 true. NULL context/running은 취소 제한이 없다는 뜻이다. */
bool io_is_active(const struct io_context *io);
/* 단조 시각 now가 절대 마감에 도달했는지 판단한다. */
bool io_deadline_passed(const struct timespec *now, const struct timespec *deadline);
/* 기존 서버의 호출별 상한·전체 마감·취소를 소켓 수신/송신 대기에 적용한다. */
int io_set_call_timeout(int fd, int option, const struct io_context *io);
/* 바이트 길이만큼 완전 송신한다. 부분 송신·EINTR 재시도 중에도 같은 마감을 유지한다.
 * 성공 0, 실패 -1(errno). 버퍼와 FD는 빌리며 닫거나 해제하지 않는다. SIGPIPE 정책은 호출자가 설정한다. */
int send_all_bytes(int fd, const void *buffer, size_t length, const struct io_context *io);
/* 1회 수신하되 EINTR·취소·전체 마감을 처리한다. recv와 같은 길이/EOF/-1 반환이며 FD는 빌린다. */
ssize_t receive_socket_bytes(int fd, void *buffer, size_t capacity, const struct io_context *io);
/* CRLFCRLF까지 읽는다. received는 본문 동반 바이트를 포함하고 header_end는 빈 줄 직후 위치다.
 * 완료 0, 조기 EOF 1, 오류 -1(errno); 읽은 바이트와 FD 소유권은 호출자에게 남는다. */
int receive_http_headers(int fd, unsigned char *buffer, size_t capacity,
                          size_t *received, size_t *header_end, const struct io_context *io);

/* FD를 빌려 exec 시 자동으로 닫히게 설정한다. 성공 0, 실패 -1(errno); FD는 닫지 않는다. */
int set_fd_close_on_exec(int fd);
/* 현재 단조 시각부터 timeout_ms 뒤의 마감을 만든다. 성공 true, 실패 false(errno). */
bool make_io_deadline(struct timespec *deadline, int timeout_ms);
/* POLLIN/POLLOUT 또는 종료·오류 사건까지 대기한다. 이후 read/SO_ERROR로 실제 결과를 확인해야 한다.
 * EINTR에도 절대 마감은 유지한다. 성공 0, 실패 -1(errno); FD 소유권은 바꾸지 않는다. */
int wait_fd_ready(int fd, short events, const struct timespec *deadline);

#endif
