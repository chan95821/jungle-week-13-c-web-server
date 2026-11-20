#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define LISTEN_BACKLOG 128
#define IO_TIMEOUT_SECONDS 5

int parse_port(const char *arg, uint16_t *out_port) {
    /* CLI 입력은 HTTP 요청과 별개다. strtol의 변환 성공 여부뿐 아니라
     * 포트 범위와 문자열 끝까지 소비했는지도 검사한다. */
    char *endptr = NULL;
    long val = 0;

    if (arg == NULL || out_port == NULL || arg[0] == '\0') {
        return -1;
    }
    if (!isdigit((unsigned char)arg[0])) {
        return -1;
    }

    errno = 0;
    val = strtol(arg, &endptr, 10);
    if (errno == ERANGE || endptr == arg || *endptr != '\0') {
        return -1;
    }
    if (val < 1 || val > 65535) {
        return -1;
    }

    *out_port = (uint16_t)val;
    return 0;
}

int open_listenfd(uint16_t port) {
    /* socket/bind/listen/setsockopt/close는 POSIX API다.
     * open_listenfd는 이 준비 과정을 묶은 우리 함수(CS:APP 관례 이름)다.
     * 성공하면 main에 FD 소유권을 넘기고, 실패하면 여기서 확보한 FD를 닫는다. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int recv_request_headers(int fd, char *buf, size_t cap,
                         size_t *out_len, size_t *out_header_len) {
    size_t total = 0;

    if (out_len != NULL) *out_len = 0;
    if (out_header_len != NULL) *out_header_len = 0;
    if (buf == NULL || cap < 4 || out_len == NULL || out_header_len == NULL) {
        errno = EINVAL;
        return -1;
    }

    while (total < cap) {
        /* TCP는 바이트 순서와 전달을 담당하지만 HTTP 메시지 경계는 모른다.
         * recv 한 번에 요청 일부만 오거나 헤더와 본문이 함께 올 수 있으므로
         * 지금까지 받은 길이 total을 기준으로 이어 쓴다. NUL 종료는 필요 없다. */
        ssize_t n = recv(fd, buf + total, cap - total, 0);
        if (n < 0) {
            /* EINTR도 상위로 전달한다. main의 종료 시그널이 진행 중인 수신을
             * 중단했을 때 무조건 재시도하면 종료 플래그를 확인하지 못하기 때문이다. */
            return -1;
        }
        if (n == 0) {
            errno = 0; /* 정상적인 peer EOF와 recv 오류를 호출자가 구분할 수 있게 함. */
            return -1;
        }

        size_t prev_total = total;
        total += (size_t)n;
        *out_len = total;

        if (total >= 4) {
            /* 직전 수신의 마지막 3바이트부터 다시 검색해야 두 recv 사이에 걸친
             * 4바이트 구분자도 찾는다. 그 앞은 이미 검색했으므로 반복하지 않는다. */
            size_t start = (prev_total >= 3) ? prev_total - 3 : 0;
            for (size_t i = start; i + 3 < total; i++) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    *out_header_len = i + 4;
                    return 0;
                }
            }
        }
    }

    return -2; /* 버퍼가 가득 찼는데도 헤더 끝이 없다: 431 응답 대상. */
}

int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len) {
    /* Iteration 1 API 호환: 실패 원인 구분과 헤더 경계가 필요 없는 기존 호출자용. */
    size_t total = 0;
    size_t header_length = 0;
    int result = recv_request_headers(fd, buf, cap, &total, &header_length);
    if (result == 0 && out_len != NULL) *out_len = total;
    return result == 0 ? 0 : -1;
}

int recv_request_body(int fd, struct http_request *request,
                      const void *initial, size_t initial_length) {
    /* Content-Length를 검증한 뒤에만 할당한다. GET에도 길이가 선언돼 있으면
     * 그만큼 읽는다. GET이라는 이유로 연결에 본문을 남겨 놓지 않는다. */
    if (request->content_length > REQ_BODY_MAX_BYTES) return 413;
    if (request->content_length == 0) return 0;
    request->body = malloc(request->content_length);
    if (request->body == NULL) return 500;

    /* 예: 헤더+"hel"까지 함께 받았다면 initial_length=3이다.
     * Content-Length=5이면 먼저 hel을 복사한 다음 lo 2바이트만 더 읽는다.
     * 선언된 길이 이후의 데이터는 다음 요청일 수 있어 이번 본문에 포함하지 않는다.
     * 이번 단계에서는 keep-alive를 지원하지 않으므로 남는 요청은 처리하지 않는다. */
    size_t copied = initial_length < request->content_length ? initial_length : request->content_length;
    if (copied > 0) memcpy(request->body, initial, copied);
    request->body_length = copied;
    while (request->body_length < request->content_length) {
        ssize_t n = recv(fd, request->body + request->body_length,
                         request->content_length - request->body_length, 0);
        if (n < 0) {
            if (errno == EINTR) return -1; /* 종료 요청: 응답 없이 정리 경로로 복귀. */
            return errno == EAGAIN || errno == EWOULDBLOCK ? 408 : 400;
        }
        if (n == 0) return 400; /* 선언한 길이를 다 받기 전 EOF: 완성된 요청이 아님. */
        request->body_length += (size_t)n;
    }
    return 0;
}

int send_all(int fd, const void *buf, size_t len) {
    const uint8_t *ptr = (const uint8_t *)buf;
    size_t sent = 0;

    if (len == 0) {
        return 0;
    }
    if (buf == NULL) {
        return -1;
    }

    while (sent < len) {
        /* send 성공도 부분 전송일 수 있다. OS가 처리했다고 반환한 길이만큼
         * 건너뛰고 나머지를 계속 보낸다. 성공은 상대 애플리케이션의 소비 보장은 아니다. */
        ssize_t n = send(fd, ptr + sent, len - sent, 0);
        if (n < 0) {
            /* 종료 시그널로 중단되면 호출자에게 돌아가 자원을 정리한다. */
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

static int send_response(int fd, const struct http_response *response) {
    /* 본문 전체를 헤더 버퍼에 넣지 않는다. 1 MiB echo도 작은 헤더와 별도로 보낸다.
     * TCP에는 send 호출 경계가 없으므로 헤더/본문을 따로 보내도 연속된 응답이다. */
    char header[512];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "%s%s%s\r\n",
                          response->status, http_status_reason(response->status),
                          response->content_type, response->body_length,
                          response->allow != NULL ? "Allow: " : "",
                          response->allow != NULL ? response->allow : "",
                          response->allow != NULL ? "\r\n" : "");
    if (length < 0 || (size_t)length >= sizeof(header)) return -1;
    if (send_all(fd, header, (size_t)length) < 0) return -1;
    return send_all(fd, response->body, response->body_length);
}

/* 이 함수는 연결 하나의 처리 순서만 조율한다.
 * main → 이 함수 → 헤더 수신 → 파싱 → 본문 수신 → 라우팅 → 응답 전송.
 * connfd는 main에서 빌린 FD이므로 여기서는 close하지 않는다.
 * 동적 본문 메모리는 여기에서 소유하고 모든 경로가 끝나는 cleanup에서 free한다. */
void handle_client(int connfd) {
    char buffer[REQ_HEADER_MAX_BYTES];
    size_t received = 0;
    size_t header_length = 0;
    struct http_request request = {0};
    struct http_response response;
    struct timeval timeout = { IO_TIMEOUT_SECONDS, 0 };
    if (setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt(client timeout)");
        return;
    }

    int result = recv_request_headers(connfd, buffer, sizeof(buffer), &received, &header_length);
    int status = 0;
    if (result != 0) {
        /* 접속 확인용 연결처럼 아무 데이터 없이 닫은 상대에는 응답하지 않는다.
         * 오류 원인 errno는 다른 함수 호출 전에 확인해야 한다. */
        if (result == -1 && (errno == EINTR || received == 0)) goto cleanup;
        status = result == -2 ? 431 :
                 (errno == EAGAIN || errno == EWOULDBLOCK ? 408 : 400);
        goto respond;
    }
    status = parse_http_request(buffer, header_length, &request);
    if (status != 0) goto respond;

    /* buffer의 배치: [헤더 header_length바이트][이미 받은 본문 ...].
     * received와 header_length를 구별하는 것이 동반 도착 처리의 핵심이다. */
    status = recv_request_body(connfd, &request, buffer + header_length, received - header_length);
    if (status == -1) goto cleanup;
    if (status != 0) goto respond;
    route_request(&request, &response);

respond:
    if (status != 0) make_error_response(status, &response);
    /* echo 응답은 request.body를 빌려 쓰므로 전송이 끝나기 전에 free하면 안 된다.
     * 전송 실패해도 연결 단위 오류이며 main은 다음 클라이언트를 계속 받는다. */
    (void)send_response(connfd, &response);
cleanup:
    free(request.body); /* NULL도 free 가능: 파싱/할당 이전 실패도 같은 경로 사용. */
}
