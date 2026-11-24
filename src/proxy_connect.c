#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "io_utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* 개인 검증 기본값. DNS 조회는 하지 않아 이 제한 밖에서 이름 조회로 대기하지 않는다. */
#define BACKEND_CONNECT_TIMEOUT_MS 1000

static int prepare_backend_address(const struct proxy_backend *backend,
                                    struct sockaddr_storage *address, socklen_t *length);
static int wait_backend_connection(int fd, const struct timespec *deadline);
static int backend_connect_error_status(int error);

/* acquire_proxy_backend()가 고른 숫자 IP 한 곳에만 제한 시간 내 연결한다(선택·우회·HTTP 송신 없음).
 * 성공 시 blocking/CLOEXEC FD를 호출자에게 넘긴다. 모든 실패는 내부 FD를 닫고 *backend_fd=-1을 유지한다.
 * 설정·로컬 자원 오류 500, 원격 연결 실패 502, 연결 제한 초과 504를 반환한다.
 * fcntl 기반 CLOEXEC는 현재 순차 처리 전제다. 스레드 동시성에서는 생성·exec 경합을 재검토해야 한다. */
int connect_backend(const struct proxy_backend *backend, int *backend_fd) {
    *backend_fd = -1;
    struct sockaddr_storage address;
    socklen_t length;
    int status = prepare_backend_address(backend, &address, &length);
    if (status) return status;

    struct timespec deadline;
    if (!make_io_deadline(&deadline, BACKEND_CONNECT_TIMEOUT_MS)) return 500;
    int fd = socket(address.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return 500;
    int flags = fcntl(fd, F_GETFL);
    status = 500;
    if (flags < 0 || set_fd_close_on_exec(fd) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) goto failure;

    if (connect(fd, (struct sockaddr *)&address, length) < 0) {
        if (errno == EINPROGRESS || errno == EINTR) {
            status = wait_backend_connection(fd, &deadline);
        } else {
            status = backend_connect_error_status(errno);
        }
        if (status) goto failure;
    }

    status = 500;
    if (fcntl(fd, F_SETFL, flags) < 0) goto failure;
    *backend_fd = fd;
    return 0;

failure:
    close(fd);
    return status;
}

/* DNS 조회 없이 숫자 IPv4/IPv6와 포트를 소켓 주소로 변환한다.
 * 설정 파일은 시작 시 이미 주소를 검증하지만, 직접 구성한 업스트림도 있을 수 있어 여기서 한 번 더 확인한다.
 * 형식 오류는 500(설정·로컬 오류)이다. */
static int prepare_backend_address(const struct proxy_backend *backend,
                                    struct sockaddr_storage *address, socklen_t *length) {
    if (!backend || !backend->host || backend->port == 0) return 500;
    memset(address, 0, sizeof(*address));
    struct sockaddr_in ipv4 = {.sin_family = AF_INET, .sin_port = htons(backend->port)};
    if (inet_pton(AF_INET, backend->host, &ipv4.sin_addr) == 1) {
        memcpy(address, &ipv4, sizeof(ipv4));
        *length = sizeof(ipv4);
        return 0;
    }
    struct sockaddr_in6 ipv6 = {.sin6_family = AF_INET6, .sin6_port = htons(backend->port)};
    if (inet_pton(AF_INET6, backend->host, &ipv6.sin6_addr) == 1) {
        memcpy(address, &ipv6, sizeof(ipv6));
        *length = sizeof(ipv6);
        return 0;
    }
    return 500;
}

/* 쓰기 가능은 연결 성공 자체가 아니다. poll 후 SO_ERROR를 확인해 실제 연결 결과를 반환한다.
 * FD를 빌리며 실패해도 닫지 않는다. 상위 connect_backend()가 정리를 맡는다. */
static int wait_backend_connection(int fd, const struct timespec *deadline) {
    if (wait_fd_ready(fd, POLLOUT, deadline) < 0) return errno == ETIMEDOUT ? 504 : 500;
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return 500;
    return error ? backend_connect_error_status(error) : 0;
}

/* 로컬 실행 실패와 원격 연결 장애를 구분해 내부 오류로 서버를 제외하지 않게 한다. */
static int backend_connect_error_status(int error) {
    if (error == ETIMEDOUT) return 504;
    if (error == ENOMEM || error == ENOBUFS || error == EINVAL ||
        error == EBADF || error == ENOTSOCK || error == EADDRNOTAVAIL) return 500;
    return 502;
}
