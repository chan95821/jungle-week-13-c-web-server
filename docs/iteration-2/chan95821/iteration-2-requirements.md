# Iteration 2 요구사항 및 구현 경계

기준 문서: [Issue #2](https://github.com/chan95821/jungle-week-13-c-web-server/issues/2), [DEVELOPMENT_CONTEXT.md](../../DEVELOPMENT_CONTEXT.md), [web-server.md](../../requirements/web-server.md)

---

## 1. 목표

TCP 바이트 스트림에서 HTTP/1.1 요청 라인·헤더·본문 경계를 직접 해석하고, 요청 파싱과 라우팅/응답 생성을 분리하여 `GET` / `POST` 동적 응답 및 오류 응답을 안전하게 반환한다.

---

## 2. 필수 구현 요구사항 (In-Scope)

### A. 요청 수신 및 프레이밍
- **헤더 수신과 본문 앞부분 분리**:
  - `recv_until_header_end()`로 `\r\n\r\n`까지 수신하되, 헤더 끝 오프셋(`header_len`)과 실제 수신 총량(`total_len`)을 분리 반환한다.
  - 한 번의 `recv()`에 헤더와 본문 일부가 함께 도착한 경우, `\r\n\r\n` 이후 바이트(`excess`)를 버리지 않고 본문 수신으로 넘긴다.
- **메서드 비종속적 본문 수신**:
  - `GET`/`POST`별 별도 수신기를 두지 않고, 파싱된 프레이밍 헤더(`Content-Length`)에 따라 본문을 수신한다.
  - 본문이 여러 `recv()`에 나뉘어 도착해도 `Content-Length`만큼 정확히 수신하며, `\0` 바이트가 포함된 바이너리 데이터도 안전하게 처리한다.
- **크기 상한**:
  - 헤더 전체 최대 크기: `REQ_HEADER_MAX_BYTES` (`8192` 바이트)
  - URI 최대 길이: `REQ_URI_MAX_BYTES` (`2048` 바이트)
  - 본문 최대 크기: `REQ_BODY_MAX_BYTES` (`1048576` 바이트, 1 MiB)

### B. HTTP 요청 파싱
- **엄격한 파싱 원칙 (Postel's Law 비적용)**:
  - HTTP Request Smuggling, CRLF Injection, 파서 불일치를 막기 위해 비표준 입력을 관대하게 보정하지 않고 명시적으로 `400 Bad Request`로 거부한다.
  - 줄 구분자는 `\r\n`(CRLF)만 허용하며, 줄 내부에 **단독 `\n`(LF), 단독 `\r`(CR)**이 남아 있으면 `400`으로 거부한다.
- **허용/금지 문자 규칙 (RFC 9110 / RFC 9112 근거)**:
  - **제어 문자 전면 금지**: 헤드 전체에서 `\0`(`0x00`)을 포함한 제어 문자(`0x00`~`0x1F`, `0x7F` DEL)를 금지한다. *(줄 구분자 `\r\n`과 헤더 값 내 `HTAB(0x09)`만 예외)*
    - 근거: `\0`은 C 문자열 조기 종료로 인한 파서 우회를 유발하고, `\r`/`\n`은 CRLF Injection을, 기타 제어 문자는 파서·로그 오작동을 유발한다.
  - **Method & `field-name` (`token` = `1*tchar`, RFC 9110 §5.6.2)**:
    - `!#$%&'*+-.^_`|~` 및 영숫자(`0-9`, `A-Z`, `a-z`)만 허용한다. 공백, 구분자(`()<>@,;:\"/[]?={}`), 제어 문자, 비-ASCII(`0x80`~`0xFF`)는 모두 `400`으로 거부한다. (콜론 앞 공백 `Name : Val`도 자동 차단)
  - **Request-Target (`URI`)**:
    - `'/'`로 시작하는 origin-form만 허용하며, 가시 ASCII(`VCHAR`: `0x21`~`0x7E`)만 허용한다. 공백, 제어 문자, 비-ASCII(`0x80`~`0xFF`) 포함 시 `400`으로 거부한다.
  - **헤더 값 (`field-value`, RFC 9110 §5.5)**:
    - `VCHAR`(`0x21`~`0x7E`), `SP`(`0x20`), `HTAB`(`0x09`)만 허용한다. 제어 문자 및 deprecated된 비-ASCII(`obs-text`: `0x80`~`0xFF`)는 `400`으로 거부한다.
- **요청 라인 (`Method URI Version\r\n`)**:
  - 단일 공백(`' '`) 기준 3토큰 분리 및 형식 검증.
  - 허용 버전: `HTTP/1.1`, `HTTP/1.0`
- **헤더 파싱 (문법 분리 + 의미 해석)**:
  - `parse_header_line()`으로 `field-name`과 `field-value`를 문법적으로 분리한다 (`:` 누락, 빈 이름, `tchar` 위반, `field-value` 금지 문자 거부, value 앞뒤 `' '`/`'\t'` 트림).
  - 헤더 이름은 대소문자를 구분하지 않는다 (`strncasecmp`).
  - 지원·검증 대상 헤더:
    - `Host`: **HTTP/1.1에서는 필수**이며 누락 시 `400`으로 거부한다. (`HTTP/1.0`은 누락 허용) 빈 값, 중복 `Host`, 길이 상한(`260B`: DNS 253B + `:65535` + `\0`) 초과 시 `400`으로 거부한다.
    - `Content-Length`: 음이 아닌 정수만 허용하며, 비숫자/음수/오버플로우를 거부한다. 중복 등장 시(동일 값 포함) `400`으로 거부한다.
    - `Transfer-Encoding`: `chunked` 등 미지원 인코딩이 오면 `501`로 거부한다. 빈 값은 `400`, `Content-Length`와 동시에 존재하면 경계 모호성 방지를 위해 `400`으로 거부한다.
    - `Connection`: `close` / `keep-alive` 여부를 파싱한다.
    - `Content-Type`: 요청 본문 타입으로 파싱한다. 길이 상한(`256B`: RFC 6838 MIME 최대 기준) 초과·중복 시 `400`으로 거부한다.
  - 그 외 유효한 `Key: Value` 헤더는 문법 검증 후 안전하게 무시한다.

### C. 요청·응답 구조 분리 및 디스패치
- 소켓 I/O(`recv_http_request`, `send_http_response`)와 라우팅/비즈니스 로직(`dispatch_request`)을 `HttpRequest` / `HttpResponse` 구조체 경계로 분리한다. (`dispatch_request`는 소켓 `fd`에 의존하지 않는다.)
- `HttpRequest.uri`에는 query string을 포함한 원본 URI를 통째로 저장하고, `dispatch_request` 시점에만 `?` 앞부분(path)을 분리해 라우팅한다.
- **지원 라우트**:
  - `GET /` 및 `GET /hello`: `200 OK`, `Content-Type: text/plain`, `Content-Length: 5`, `Connection: close`, 본문 `Hello` *(Iteration 1 회귀 호환; `/hello?a=1` 등 query string 포함 시에도 path 기준 `/hello`로 라우팅)*
  - `POST /echo`: `200 OK`, 요청 본문(`body`, `body_len`)을 그대로 응답 본문으로 반환한다. (`Content-Length`는 실제 본문 바이트 길이로 동적 생성, `Content-Type`은 요청에 있으면 유지하고 없으면 `application/octet-stream` 사용, `Content-Length: 0` 명시 시 빈 본문 반환, `Content-Length`가 없는 `POST`는 `411 Length Required` 거부)

### D. 오류 처리 및 상태 코드 정책
- **응답 없이 즉시 연결 종료**:
  - 헤더 수신 중 `8192B` 상한 초과, `\r\n\r\n` 도달 전 조기 `EOF`, 네트워크 오류, 본문 수신 중 조기 `EOF` *(Iteration 1 회귀 기준 유지)*
- **HTTP 오류 응답 반환 후 `Connection: close` 종료**:
  - 상태 코드별 `(status, reason, body)` 매핑 테이블로 분리 보관하며, `Content-Type: text/plain`과 짧은 설명 본문(예: `"404 Not Found\n"`)을 함께 반환한다. (RFC 9110 권장)
  - `400 Bad Request`: 잘못된 요청 라인/헤더 문법, HTTP/1.1 `Host` 누락, 잘못된/중복 `Content-Length`, `Host`/`Content-Type` 상한 초과·중복, `Transfer-Encoding` + `Content-Length` 동시 존재
  - `404 Not Found`: 지원하지 않는 URI 경로
  - `405 Method Not Allowed`: `GET`, `POST` 외의 메서드 (예: `PUT`, `DELETE`, `HEAD`), 또는 경로는 존재하나 해당 메서드가 허용되지 않은 경우 (`GET /echo`, `POST /hello`)
  - `411 Length Required`: `POST` 요청에 `Content-Length` 헤더가 누락된 경우
  - `413 Payload Too Large`: `Content-Length`가 `REQ_BODY_MAX_BYTES` 초과
  - `414 URI Too Long`: `URI` 길이가 `REQ_URI_MAX_BYTES` 초과
  - `501 Not Implemented`: `Transfer-Encoding: chunked` 등 미지원 전송 인코딩
  - `505 HTTP Version Not Supported`: `HTTP/1.1`, `HTTP/1.0` 외의 버전 (예: `HTTP/2.0`)

### E. 응답 송신 및 자원 수명
- 모든 응답에는 `Connection: close` 헤더를 포함하고 송신 완료 후 `connfd`를 닫는다. (클라이언트가 `Connection: keep-alive`를 보내도 동일하게 `Connection: close`로 응답하고 연결을 종료한다.)
- 헤더와 본문은 `send_all()`을 사용해 명시적 바이트 길이(`size_t`) 기준으로 송신한다.
- 동적 할당된 본문 버퍼는 `handle_client()`의 단일 정리 지점(`free_http_request`, `free_http_response`)에서 모든 정상·오류 경로를 포함해 반드시 해제한다.
- 요청/연결 상태를 전역 공유 버퍼에 두지 않으며, `SIGPIPE` 무시 및 `SIGINT`/`SIGTERM` 안전 종료를 유지한다.

---

## 3. 구현 경계 / 비범위 (Out-of-Scope)

Iteration 2에서는 아래 항목을 구현하지 않는다:
- `Transfer-Encoding: chunked` 디코딩 *(감지 시 `501` 거부)*
- `Connection: keep-alive` 지속 연결 *(항상 `Connection: close` 응답 후 종료)*
- 정적 파일 서빙, MIME 타입 매핑, 경로 탐색(`../`) 방어 *(→ Iteration 3)*
- Reverse Proxy *(→ Iteration 4; Forward Proxy 및 HTTPS `CONNECT`는 전체 범위 제외)*
- 동시 요청 처리 (`fork`, `pthread`, `select`/`poll`/`epoll`/`kqueue`) *(→ Iteration 5)*

---

## 4. 완료 조건 (Definition of Done) 및 반례 기준

- `make` 빌드 성공, `make test` (`-fsanitize=address,undefined`) 단위 테스트 전체 통과
- `make verify`에서 **Iteration 1 회귀 테스트 + Iteration 2 E2E 검증** 모두 통과:
  1. `GET /hello`, `GET /` → `200 OK` (`Hello`)
  2. `POST /echo` → 요청 본문(`text`, 바이너리, 빈 본문) 그대로 `200 OK` 반환
  3. 헤더·본문이 여러 `recv()`에 분할되거나 한 `recv()`에 합쳐져 도착해도 정상 처리
  4. 잘못된 요청 라인/헤더(`400`), 미존재 경로(`404`), 미지원 메서드(`405`), 본문 상한 초과(`413`), 긴 URI(`414`), `chunked`(`501`), 미지원 버전(`505`) 안전 거부
  5. 본문 수신 중 조기 `EOF` 시 메모리 누수 없이 연결 정리 및 서버 생존
