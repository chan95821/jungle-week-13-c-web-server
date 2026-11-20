#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* 서로 다른 상한이다. 헤더 크기에는 마지막 CRLFCRLF까지 포함하고,
 * 본문 크기는 Content-Length의 바이트 수이다. NUL 종료 공간은 포함하지 않는다. */
#define REQ_HEADER_MAX_BYTES 8192
#define REQ_TARGET_MAX_BYTES 2048
#define REQ_BODY_MAX_BYTES (1024 * 1024)
#define HELLO_NAME_MAX_BYTES 256

enum http_method { HTTP_GET, HTTP_POST, HTTP_OTHER };

/* 파서가 채우는 요청 메타데이터 + 수신 계층이 채우는 본문.
 * target/host는 C 문자열이지만 body는 문자열이 아니다. 반드시 body_length를 쓴다.
 * body의 소유자는 handle_client이며 route_request는 이를 빌려서 읽기만 한다. */
struct http_request {
    enum http_method method;
    char target[REQ_TARGET_MAX_BYTES + 1];
    char host[256];
    int http11;
    int connection_close;
    int has_content_length;
    size_t content_length;
    unsigned char *body;
    size_t body_length;
};

/* 응답 본문은 세 가지 중 하나를 가리킨다:
 * 1) 문자열 상수(오류/루트), 2) text(동적 인사), 3) 요청 body(echo).
 * 따라서 이 구조체의 body를 free하지 않는다. 원본은 전송 종료까지 살아 있어야 한다.
 * text를 가리키는 포인터 때문에 완성된 구조체를 값 복사해 사용하지 않는다. */
struct http_response {
    int status;
    const char *content_type;
    const void *body;
    size_t body_length;
    const char *allow;
    char text[HELLO_NAME_MAX_BYTES + 32];
};

/* 아래 함수는 우리가 작성한 HTTP 처리 함수이며 소켓을 직접 읽거나 닫지 않는다.
 * parse_http_request: 완성된 헤더만 받는다. 성공 0, 실패 HTTP 상태 코드 반환.
 * out은 성공/실패에 관계없이 초기화된다. 본문 메모리를 가진 요청에 재호출하지 않는다. */
int parse_http_request(const void *header, size_t length, struct http_request *out);
void make_error_response(int status, struct http_response *out);
void route_request(const struct http_request *request, struct http_response *out);
const char *http_status_reason(int status);

#endif /* HTTP_H */
