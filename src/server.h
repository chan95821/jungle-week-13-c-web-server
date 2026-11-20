#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>
#include "http.h"

int parse_port(const char *arg, uint16_t *out_port);
int open_listenfd(uint16_t port);
int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len);
/* 확장 수신 함수: out_len은 함께 수신한 본문을 포함한 길이,
 * out_header_len은 CRLFCRLF까지의 길이다. 0 성공, -1 I/O/EOF, -2 상한 초과.
 * 기존 recv_until_header_end는 이전 단위 테스트/API 호환용 래퍼다. */
int recv_request_headers(int fd, char *buf, size_t cap,
                         size_t *out_len, size_t *out_header_len);
/* request의 메타데이터는 파싱 완료 상태여야 한다. initial은 이미 받은 본문.
 * 성공 0, 실패 HTTP 상태 코드. 할당된 body는 실패해도 호출자가 free한다. */
int recv_request_body(int fd, struct http_request *request,
                      const void *initial, size_t initial_length);
int send_all(int fd, const void *buf, size_t len);
void handle_client(int connfd);

#endif /* SERVER_H */
