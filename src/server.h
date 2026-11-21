#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>

#define REQ_HEADER_MAX_BYTES       8192
#define REQ_URI_MAX_BYTES          2048
#define REQ_BODY_MAX_BYTES         (1024 * 1024) /* 1 MiB */
#define REQ_METHOD_MAX_BYTES       16
#define REQ_VERSION_MAX_BYTES      16
#define REQ_HOST_MAX_BYTES         260           /* DNS 253B + :65535 + NUL */
#define REQ_CONTENT_TYPE_MAX_BYTES 256           /* RFC 6838 MIME 최대 기준 */

typedef struct {
    char method[REQ_METHOD_MAX_BYTES];
    char uri[REQ_URI_MAX_BYTES + 1];
    char version[REQ_VERSION_MAX_BYTES];
    char host[REQ_HOST_MAX_BYTES];
    char content_type[REQ_CONTENT_TYPE_MAX_BYTES];
    size_t content_length;
    int has_host;
    int has_content_length;
    int has_content_type;
    int is_chunked;
    int connection_close;
    uint8_t *body;      /* recv_body에서 힙 할당, free_http_request에서 해제 */
    size_t body_len;
} HttpRequest;

typedef enum {
    HTTP_STATUS_NONE                       = 0,
    HTTP_STATUS_OK                         = 200,
    HTTP_STATUS_BAD_REQUEST                = 400,
    HTTP_STATUS_NOT_FOUND                  = 404,
    HTTP_STATUS_METHOD_NOT_ALLOWED         = 405,
    HTTP_STATUS_LENGTH_REQUIRED            = 411,
    HTTP_STATUS_PAYLOAD_TOO_LARGE            = 413,
    HTTP_STATUS_URI_TOO_LONG                 = 414,
    HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
    HTTP_STATUS_INTERNAL_SERVER_ERROR        = 500,
    HTTP_STATUS_NOT_IMPLEMENTED            = 501,
    HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED = 505
} HttpStatus;

typedef struct {
    HttpStatus status_code;
    const char *reason;
    const char *content_type;
    const uint8_t *body;
    size_t body_len;
    int owns_body;      /* 1이면 free_http_response에서 해제, 0이면 비소유 참조 */
} HttpResponse;

/* CLI 포트 문자열을 검증해 1..65535 범위 포트를 반환한다. (실패 시 -1) */
int  parse_port(const char *arg, uint16_t *out_port);
/* SO_REUSEADDR 설정 후 바인딩·리스닝 소켓 fd를 생성한다. (실패 시 -1) */
int  open_listenfd(uint16_t port);
/* len 바이트 전체를 끝까지 송신한다. (부분 송신/EINTR 처리, 실패 시 -1) */
int  send_all(int fd, const void *buf, size_t len);
/* 단일 클라이언트 연결의 요청 수신·디스패치·응답 송신·자원 해제를 조율한다. */
void handle_client(int connfd);

/* \r\n\r\n까지 수신하고 헤더 끝(out_header_len)과 총 수신량(out_total_len)을 분리한다. (상한 초과 -2, 오류/EOF -1) */
int recv_until_header_end(int fd, char *buf, size_t cap,
                          size_t *out_header_len, size_t *out_total_len);
/* 헤더 직후 이미 도착한 excess를 복사한 뒤 남은 content_length만큼 수신한다. (호출자가 free) */
int recv_body(int fd, const char *excess, size_t excess_len,
              size_t content_length, uint8_t **out_body, size_t *out_len);

/* 요청 라인(Method SP URI SP Version)을 분리·검증한다. (실패 시 http_status 설정) */
int parse_request_line(const char *line, size_t line_len,
                       HttpRequest *req, HttpStatus *http_status);
/* 헤더 한 줄을 문법 검증 후 Zero-copy (name, val) 슬라이스로 분리한다. (의미 해석 안 함) */
int parse_header_line(const char *line, size_t line_len,
                      const char **out_name, size_t *out_name_len,
                      const char **out_val, size_t *out_val_len);
/* 헤더 줄들을 순회 파싱하고 Host 필수 여부 및 프레이밍 일관성을 검증한다. */
int parse_headers(const char *headers, size_t headers_len,
                  HttpRequest *req, HttpStatus *http_status);
/* 헤드 바이트를 일괄 검증한 뒤 요청 라인과 헤더 전체를 파싱한다. */
int parse_request_head(const char *buf, size_t header_len,
                       HttpRequest *req, HttpStatus *http_status);

/* 소켓에서 요청 헤드·본문을 수신해 req를 채운다. (무응답 종료 시 *http_status=HTTP_STATUS_NONE) */
int  recv_http_request(int connfd, HttpRequest *req, HttpStatus *http_status);
/* 상태 코드 매핑 테이블에서 오류 응답(reason, text/plain 본문)을 구성한다. */
void set_error_response(HttpResponse *resp, HttpStatus status);
/* 소켓 fd 없이 완성된 req를 라우팅해 resp를 채운다. (? 앞 path 기준 분기) */
void dispatch_request(const HttpRequest *req, HttpResponse *resp);
/* resp의 상태 라인·헤더(Connection: close 포함)·본문을 끝까지 송신한다. */
int  send_http_response(int connfd, const HttpResponse *resp);
/* req->body 힙 메모리를 해제하고 포인터를 초기화한다. */
void free_http_request(HttpRequest *req);
/* resp->owns_body가 1인 경우에만 resp->body를 해제한다. */
void free_http_response(HttpResponse *resp);

#endif /* SERVER_H */
