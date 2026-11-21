# Iteration 2 상세 설계

기준 문서: [iteration-2-requirements.md](./iteration-2-requirements.md), [iteration-2-tests.md](./iteration-2-tests.md)

---

## 1. 아키텍처 개요 및 계층 분리

Iteration 2는 콜백 테이블 같은 과도한 추상화 없이, 역할이 다른 3개 소스 파일(`src/server.c`, `src/http_parse.c`, `src/router.c`)과 구조체 경계로 3개 계층을 분리한다. (`src/main.c`는 변경하지 않는다.)

- `src/server.c`: 소켓 I/O, 프레이밍(`recv_until_header_end`, `recv_body`), 연결 조율(`handle_client`, `recv_http_request`, `send_http_response`)
- `src/http_parse.c`: 요청 라인 및 헤더 엄격 파싱(`parse_request_line`, `parse_header_line`, `parse_headers`, `parse_request_head`)
- `src/router.c`: 라우팅 디스패치(`dispatch_request`) 및 상태 코드별 오류 응답 테이블(`set_error_response`)

```text
main.c (accept 루프, 시그널)
 └── handle_client(connfd)  [단일 연결 조율 + 단일 cleanup 지점]
      ├── [1. 프로토콜 수신/파싱] recv_http_request(connfd, &req, &err_status)
      │    ├── recv_until_header_end(...)     ──▶ header_len / total_len 분리
      │    ├── parse_request_head(...)        ──▶ parse_request_line + parse_headers
      │    │                                      (parse_header_line ↔ apply_header_field 분리)
      │    └── recv_body(...)                 ──▶ excess 복사 + 잔여 Content-Length 수신
      ├── [2. 라우팅/비즈니스]   dispatch_request(&req, &resp)  (connfd 비의존)
      │    ├── handle_get_hello(&resp)
      │    ├── handle_post_echo(&req, &resp)
      │    └── set_error_response(&resp, status, reason)
      └── [3. 프로토콜 송신]     send_http_response(connfd, &resp)
           └── send_all(...)                  ──▶ 직렬화 헤더 송신 → 본문 송신
```

- **표준 C / POSIX API (OS/라이브러리 제공)**: `recv`, `send`, `close`, `strncasecmp`(`<strings.h>`), `strtoull`, `memcpy`, `memcmp`, `snprintf`, `malloc`, `free`
- **직접 구현 함수**: 위 다이어그램의 프레이밍, 파싱, 디스패치, 응답 직렬화 함수 일체

---

## 2. 데이터 구조 및 상수 (`src/server.h`)

```c
#define REQ_HEADER_MAX_BYTES       8192
#define REQ_URI_MAX_BYTES          2048
#define REQ_BODY_MAX_BYTES         (1024 * 1024) /* 1 MiB */
#define REQ_METHOD_MAX_BYTES       16
#define REQ_VERSION_MAX_BYTES      16
#define REQ_HOST_MAX_BYTES         260           /* RFC 1035 253B + :65535 + \0 */
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
    uint8_t *body;      /* 힙 할당 (recv_body 소유, free_http_request에서 해제) */
    size_t body_len;
} HttpRequest;

typedef struct {
    int status_code;
    const char *reason;
    const char *content_type;
    const uint8_t *body;
    size_t body_len;
    int owns_body;      /* 1이면 free_http_response에서 body 해제, 0이면 비소유 참조 */
} HttpResponse;
```

- **메모리 소유권 규칙**:
  - `HttpRequest.body`는 `recv_body()`에서만 `malloc`하고, `free_http_request()`에서만 `free`한다.
  - `POST /echo`는 `resp->body = req->body`, `resp->owns_body = 0`으로 **zero-copy 참조**하므로, `double free`나 불필요한 본문 복사가 발생하지 않는다.
  - `HttpRequest`, `HttpResponse`는 모두 `handle_client()` 스택 지역 변수로 선언해 **전역 공유 버퍼를 두지 않는다**.

---

## 3. 탑다운 함수 명세

### Level 1: `void handle_client(int connfd)`
- `HttpRequest req`, `HttpResponse resp`를 `0`으로 초기화한다.
- `recv_http_request(connfd, &req, &err_status)`를 호출한다.
  - 실패 시:
    - `err_status == 0` (네트워크 오류, 조기 `EOF`, 헤더 `8192B` 초과): 응답을 보내지 않고 `cleanup`으로 이동한다. *(Iteration 1 회귀 호환)*
    - `err_status > 0` (`400`, `413`, `414`, `501`, `505`): `set_error_response(&resp, err_status, ...)` 후 `send_http_response()` 호출.
  - 성공 시:
    - `dispatch_request(&req, &resp)` → `send_http_response(connfd, &resp)` 순서로 실행한다.
- **단일 `cleanup` 지점**: 함수 끝에서 `free_http_response(&resp)` 및 `free_http_request(&req)`를 반드시 호출한다.

### Level 2-A: 요청 수신 (`recv_http_request`)
```c
int recv_http_request(int connfd, HttpRequest *req, int *out_err_status);
```
1. 스택 버퍼 `char head_buf[REQ_HEADER_MAX_BYTES]`에 `recv_until_header_end(connfd, head_buf, sizeof(head_buf), &header_len, &total_len)` 호출.
   - 실패 시 `*out_err_status = 0`으로 두고 `-1` 반환.
2. `parse_request_head(head_buf, header_len, req, out_err_status)` 호출.
   - 실패 시 `*out_err_status`에 HTTP 오류 코드(`400`/`405`/`413`/`414`/`501`/`505`)를 담아 `-1` 반환.
3. `excess_len = total_len - header_len`, `excess_ptr = head_buf + header_len` 계산.
4. `recv_body(connfd, excess_ptr, excess_len, req->content_length, &req->body, &req->body_len)` 호출.
   - 실패(조기 `EOF` / 소켓 오류) 시 `*out_err_status = 0`, `-1` 반환.

### Level 3-A: 헤더 프레이밍 (`recv_until_header_end`)
```c
int recv_until_header_end(int fd, char *buf, size_t cap,
                          size_t *out_header_len, size_t *out_total_len);
```
- `recv()` 루프로 데이터를 읽으며 `\r\n\r\n`을 탐색한다 (`EINTR` 재시도).
- `\r\n\r\n`을 `buf[i..i+3]`에서 찾으면 즉시 루프를 종료하고:
  - `*out_header_len = i + 4` (헤더 블록 끝 오프셋)
  - `*out_total_len = total` (이미 들어온 본문 앞부분을 포함한 전체 바이트 수)
- `cap`까지 `\r\n\r\n`을 못 찾거나 조기 `EOF(0)` / 오류 발생 시 `-1` 반환.

### Level 3-B: 요청 헤드 파싱 (`parse_request_head`, `parse_request_line`, `parse_headers`)
- **엄격한 파싱 원칙**: Postel's Law를 따르지 않으며, 단독 `\n`·`\r`, `\0` 바이트, 콜론 앞 공백, 중복 `Host` 등 비표준 입력을 관대하게 보정하지 않고 `400`으로 거부한다.
- **`parse_request_head(buf, header_len, req, out_err_status)`**:
  1. `buf[0..header_len)` 내부에 `\0` 바이트가 있으면 `400` 반환.
  2. 첫 번째 `\r\n`을 찾아 `parse_request_line()` 호출 (줄 내부에 단독 `\n`/`\r` 있으면 `400`).
  3. 첫 `\r\n` 이후부터 마지막 빈 줄(`\r\n\r\n` 앞)까지를 `parse_headers()`에 전달.
- **`parse_request_line(line, line_len, req, out_err_status)`**:
  - 단일 공백(`' '`) 2개로 `Method`, `URI`, `Version` 분리 (연속 공백·탭·토큰 수 불일치 → `400`).
  - `Method`: `is_tchar()`로 RFC 9110 `token` 검증(위반 → `400`) 후 `GET`, `POST` 외에는 `405`.
  - `URI`: `'/'` 시작 및 전 바이트 `VCHAR`(`0x21`~`0x7E`) 검증(위반 → `400`), 길이 `> REQ_URI_MAX_BYTES`이면 `414`.
  - `Version`: `HTTP/1.1` 또는 `HTTP/1.0`만 허용. `HTTP/`로 시작하는 다른 버전은 `505`, 형식 자체가 틀리면 `400`.
- **`parse_headers(headers, headers_len, req, out_err_status)`**:
  - `\r\n` 단위로 순회하며 각 줄에 대해 2단계 분리 적용:
    1. **문법 분리** `parse_header_line(line, line_len, &name, &name_len, &val, &val_len)`: `:` 위치 탐색, `name` 전 바이트 `is_tchar()` 검증(빈 문자열·공백·제어문자·구분자 거부 → `400`), `val` 앞뒤 `' '`/`'\t'` 트림 후 내부 바이트 `VCHAR | SP | HTAB` 검증(제어문자·비-ASCII 거부 → `400`). (Zero-copy 슬라이스)
    2. **의미 해석** `apply_header_field(req, name, name_len, val, val_len, out_err_status)`:
       - `Content-Length`: 숫자·오버플로우 검증(`400`), `> REQ_BODY_MAX_BYTES` 검증(`413`), 중복 등장 시 `400`.
       - `Transfer-Encoding`: 빈 값이면 `400`, 그 외 `is_chunked = 1` 설정.
       - `Host`, `Content-Type`: 빈 값·중복·상한(`260B`/`256B`) 초과 시 `400`, 정상이면 `req`에 반영.
       - `Connection`: 대소문자 무시 파싱 후 `req->connection_close` 반영.
  - 루프 종료 후 일관성 검증:
    - `strcmp(req->version, "HTTP/1.1") == 0 && !req->has_host` → `400`
    - `is_chunked && has_content_length` → `400`
    - `is_chunked` → `501`

### Level 3-C: 본문 수신 (`recv_body`)
```c
int recv_body(int fd, const char *excess, size_t excess_len,
              size_t content_length, uint8_t **out_body, size_t *out_len);
```
- `content_length == 0`이면 `*out_body = NULL`, `*out_len = 0`, `0` 반환.
- `malloc(content_length)` 할당 (실패 시 `-1`).
- `copy_len = (excess_len < content_length) ? excess_len : content_length`만큼 `memcpy`.
- `received = copy_len`부터 `received < content_length` 동안 `recv()` 루프 (`EINTR` 재시도).
  - 도중 `n <= 0` (조기 `EOF` 또는 오류)이면 `free(buf)` 후 `-1` 반환.
- 완료 시 `*out_body = buf`, `*out_len = content_length`, `0` 반환.

### Level 2-B: 라우팅 및 비즈니스 로직 (`dispatch_request`)
```c
void dispatch_request(const HttpRequest *req, HttpResponse *resp);
```
- `req->uri`에서 `?` 앞부분(`path_len`)만 분리해 경로 비교에 사용한다.
- `POST` 요청인데 `!req->has_content_length`인 경우 → `set_error_response(resp, 411)` (`411 Length Required`)
- `path`가 `/` 또는 `/hello`인 경우:
  - `GET` → `handle_get_hello(resp)` (`200 OK`, `text/plain`, `"Hello"`)
  - 그 외 메서드 → `set_error_response(resp, 405)` (`405 Method Not Allowed`)
- `path`가 `/echo`인 경우:
  - `POST` → `handle_post_echo(req, resp)` (`200 OK`, `req->content_type` 또는 `"application/octet-stream"`, `req->body`, `req->body_len`)
  - 그 외 메서드 → `set_error_response(resp, 405)` (`405 Method Not Allowed`)
- 그 외 경로 → `set_error_response(resp, 404)` (`404 Not Found`)
- `set_error_response(resp, status)`는 정적 매핑 테이블에서 상태 코드별 `(reason, body)`를 찾아 `owns_body = 0`, `Content-Type: text/plain`으로 설정한다.

### Level 2-C: 응답 직렬화 및 송신 (`send_http_response`)
```c
int send_http_response(int connfd, const HttpResponse *resp);
```
- `snprintf`로 상태 라인 + `Content-Type` + `Content-Length: %zu` + `Connection: close` + `\r\n` 헤더를 스택 버퍼에 직렬화해 `send_all()` 호출.
- `resp->body_len > 0`이면 `send_all(connfd, resp->body, resp->body_len)` 호출.

---

## 4. 후속 Iteration 확장성 및 변경 비용

- **Iteration 3 (정적 파일 서빙)**: `dispatch_request`의 `GET` 분기 아래에 정적 파일 핸들러를 추가하면 되며, 프레이밍/파싱 코드는 변경되지 않는다.
- **Iteration 4 (Reverse Proxy)**: 백엔드 전달 시 원본 헤더 목록이 필요해지면 `HttpRequest`에 헤더 배열(또는 `raw_head` 슬라이스)만 추가하면 된다.
- **Iteration 5 (동시성 처리)**: 전역 버퍼가 없으므로 `handle_client(connfd)`를 스레드/워커로 넘기는 비용이 낮다.
