#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

static volatile sig_atomic_t g_running = 1;

/* 시그널은 일반 실행 흐름 사이에 들어온다. 핸들러에서는 플래그만 변경한다.
 * 출력/메모리 해제/FD 정리는 main의 정상 실행 흐름에 맡긴다. */
static void on_shutdown_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int setup_signals(void) {
    struct sigaction sa_stop;

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return -1;
    }

    sa_stop.sa_handler = on_shutdown_signal;
    sigemptyset(&sa_stop.sa_mask);
    sa_stop.sa_flags = 0;
    /* SA_RESTART를 켜지 않아 recv/send/poll이 EINTR로 복귀할 수 있게 한다.
     * handle_client도 EINTR을 상위로 전달하므로 종료 플래그를 확인할 수 있다. */

    if (sigaction(SIGINT, &sa_stop, NULL) < 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa_stop, NULL) < 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    /* main은 서버의 수명을 조율하고 HTTP 문법은 하위 함수에 맡긴다.
     * parse_port/open_listenfd/handle_client는 직접 구현한 함수,
     * poll/accept/close는 운영체제가 제공하는 POSIX API다. */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    uint16_t port = 0;
    if (parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return 1;
    }

    if (setup_signals() < 0) {
        perror("sigaction");
        return 1;
    }

    int listenfd = open_listenfd(port);
    if (listenfd < 0) {
        return 1;
    }

    int exit_code = 0;
    struct pollfd ready = { listenfd, POLLIN, 0 };
    while (g_running) {
        /* 종료 플래그 확인 직후 신호가 와도 무한 accept 대기에 들어가지 않는다.
         * 리스너 하나만 감시하므로 이 poll은 동시 클라이언트 처리 기능이 아니다. */
        int count = poll(&ready, 1, 250);
        if (count < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            exit_code = 1;
            break;
        }
        if (count == 0 || !g_running) continue;
        if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Listening socket failed\n");
            exit_code = 1;
            break;
        }
        if (!(ready.revents & POLLIN)) continue;
        /* listenfd는 연결 입구, connfd는 이번 상대와 통신하는 별도 소켓이다. */
        int connfd = accept(listenfd, NULL, NULL);
        if (connfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                continue;
            }
            perror("accept");
            exit_code = 1;
            break;
        }
        handle_client(connfd);
        /* FD 소유자는 main이다. 내부 수신/파싱/송신 실패도 여기로 돌아온다. */
        close(connfd);
    }

    close(listenfd);
    return exit_code;
}
