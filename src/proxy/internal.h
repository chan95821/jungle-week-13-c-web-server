#ifndef PROXY_INTERNAL_H
#define PROXY_INTERNAL_H

/* 프록시 구현 파일끼리 사용하는 내부 계약이다.
 * main/server는 기존 ../proxy.h만 포함하며 이 세부 구조를 알 필요가 없다. */
#include "../http.h"
#include <netinet/in.h>
#include <signal.h>
#include <sys/types.h>

enum {
    PROXY_BACKEND_TIMEOUT_MS = 1000,
    PROXY_CLIENT_TIMEOUT_MS = 5000,
    PROXY_BUFFER_CAPACITY = 8192,
    /* 원본 헤더에 Connection/Via 등을 재생성할 때 필요한 기존 여유 공간. */
    PROXY_WIRE_CAPACITY = PROXY_BUFFER_CAPACITY + 256
};

/* reader는 backend FD를 빌린다. close는 handle_reverse_proxy_request가 맡는다.
 * [position, length)에 아직 소비하지 않은 헤더/본문 바이트가 남아 있다. */
struct proxy_reader {
    int backend_fd;
    const volatile sig_atomic_t *running;
    unsigned char buffer[PROXY_BUFFER_CAPACITY];
    size_t position;
    size_t length;
};

/* 파싱 결과에는 길이·상태만 저장하며 원본 헤더 포인터는 보관하지 않는다. */
struct proxy_response {
    int status_code;
    bool has_content_length;
    bool is_chunked;
    size_t content_length;
    size_t status_line_length;
};

/* io.c: POSIX 소켓 대기·부분 송수신과 연결별 버퍼. HTTP 의미는 해석하지 않는다. */
bool is_server_running(const volatile sig_atomic_t *running);
int connect_to_backend(const struct sockaddr_in *address,
                          const volatile sig_atomic_t *running);
int send_all_bytes(int fd, const void *data, size_t length, int timeout_ms,
                    const volatile sig_atomic_t *running);
/* 버퍼를 소진했을 때 호출. 양수: 수신 바이트 수, 0: EOF, -1: errno. */
ssize_t receive_backend_bytes(struct proxy_reader *reader);
/* byte/line 읽기는 예상한 데이터 이전 EOF도 EPROTO로 반환한다. */
int read_backend_byte(struct proxy_reader *reader, unsigned char *byte);
int read_backend_line(struct proxy_reader *reader, unsigned char *line,
                           size_t capacity, size_t *length);

/* headers.c: 원본 헤더 검사·재구성. output 공간은 PROXY_WIRE_CAPACITY다. */
bool build_backend_request_headers(const unsigned char *header, size_t length,
                                  const struct http_request *request,
                                  unsigned char *output, size_t *output_length);
bool parse_backend_response_headers(const unsigned char *header, size_t length,
                                   struct proxy_response *response);
bool build_client_response_headers(const unsigned char *header, size_t length,
                                   const struct proxy_response *response,
                                   unsigned char *output, size_t *output_length);
/* 중간 1xx 소비 → 최종 헤더 검사 → client용 헤더 준비. 오류는 -1/errno. */
int prepare_client_response_headers(struct proxy_reader *reader,
                                    struct proxy_response *response,
                                    unsigned char *output, size_t *output_length);
/* 청크 확장/트레일러도 같은 HTTP 필드 문법으로 검사한다. */
bool is_http_token_character(unsigned char byte);
bool is_allowed_chunk_trailer_header(const unsigned char *line, size_t length);

/* body.c: 길이/청크/EOF별 프레이밍. FD를 닫거나 새 오류 응답을 보내지 않는다. */
bool parse_chunk_size_line(const unsigned char *line, size_t length, size_t *size);
int send_backend_response_body_to_client(struct proxy_reader *reader, int client_fd,
                               const struct proxy_response *response);

#endif
