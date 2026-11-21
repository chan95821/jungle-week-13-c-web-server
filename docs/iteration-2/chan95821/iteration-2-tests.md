# Iteration 2 테스트 문서

설계 단계에서 탑다운 함수 계층별로 도출한 테스트 케이스를 누적 기록한다.

---

## 1. `recv_until_header_end(fd, buf, cap, &header_len, &total_len)`

### 정상 및 경계 분리 케이스 (반환값 `0`)

| 시나리오 | 기대 결과 | 검증 포인트 |
| --- | --- | --- |
| 헤더만 1회 수신 (`GET /hello HTTP/1.1\r\n\r\n`) | `header_len == total_len` | 초과 바이트(`excess`) 없음 |
| 헤더 + 본문 동반 수신 (`POST /echo ...\r\n\r\nHello`) | `header_len < total_len`, `buf + header_len`이 `"Hello"` | `\r\n\r\n` 직후 위치와 전체 수신 길이 정확히 분리 |
| 분할 수신 (`...\r\n\r` 후 `\nHello` 도착) | `header_len`이 `\r\n\r\n` 끝, `excess`가 `"Hello"` | `recv()` 경계에 걸친 탐색 및 이후 바이트 보존 |
| 헤더 + 본문(합계 10KB) 동시 도착 (`cap = 8192` 안의 `\r\n\r\n`) | `header_len` 정상 추출, `total_len == 8192` | 본문과 합쳐 8192B를 넘겨도 헤더만 8192B 안이면 정상 추출 |
| 본문 내부에 `\r\n\r\n` 포함 (`...\r\n\r\nhello\r\n\r\nworld`) | `header_len`은 첫 `\r\n\r\n` 직후, `excess`에 `hello\r\n\r\nworld` 보존 | 본문 내 `\r\n\r\n`으로 헤더 끝을 오인하지 않음 |
| 정확히 `8192`바이트째에 `\r\n\r\n` 종료 | 성공 (`0`), `header_len == 8192` | 버퍼 상한 `off-by-one` 없음 |

### 예외 케이스 (반환값 `-1`)

| 시나리오 | 기대 결과 | 검증 포인트 |
| --- | --- | --- |
| 4바이트 미만(`"GE"`) 수신 후 조기 `EOF` | 실패 (`-1`) | `total < 4` 상태에서 언더플로우/오탐 없이 안전 종료 |
| `\r\n\r\n` 전 조기 `EOF` | 실패 (`-1`) | 불완전 헤더 거부 |
| `8192`바이트 초과까지 `\r\n\r\n` 미도달 | 실패 (`-2`) | 헤더 상한 초과 → `431` 반환 트리거 |

---

## 2. `parse_request_line(line, line_len, req, &http_status)`

### 정상 케이스 (반환값 `0`)

| 입력 라인 | 기대 결과 | 검증 포인트 |
| --- | --- | --- |
| `GET /hello HTTP/1.1` | `method="GET"`, `uri="/hello"`, `version="HTTP/1.1"` | 기본 GET 요청 라인 |
| `POST /echo HTTP/1.0` | `method="POST"`, `uri="/echo"`, `version="HTTP/1.0"` | POST 및 `HTTP/1.0` 허용 |
| `GET /<2047바이트> HTTP/1.1` (URI 총 `2048B`) | 성공 (`0`) | URI 최대 길이 경계값 |

### 예외 케이스 (반환값 `-1`, `err_status` 설정)

| 입력 라인 | 기대 `err_status` | 검증 포인트 |
| --- | --- | --- |
| `""` (빈 줄) | `400` | 빈 요청 라인 거부 |
| `GET /hello` (토큰 2개) | `400` | 버전 누락 |
| `GET /hello HTTP/1.1 extra` (토큰 4개) | `400` | 여분 토큰 거부 |
| `GET  /hello HTTP/1.1` (연속 공백) / 탭 구분 | `400` | 엄격한 단일 `' '` 구분자 검증 |
| `GET hello HTTP/1.1` (`/`로 시작 안 함) | `400` | 잘못된 request-target 형식 |
| `GET /he\x7fllo HTTP/1.1` / `GET /한글 HTTP/1.1` | `400` | URI 내 제어 문자(`0x7F`)·비-ASCII(`0x80`~`0xFF`) 거부 |
| `PUT /echo HTTP/1.1` / `DELETE /hello HTTP/1.1` | `405` | 유효 토큰이지만 미지원 메서드 |
| `BAD!METH / HTTP/1.1` | `400` | 비정상 메서드 토큰 |
| `GET /<2048바이트> HTTP/1.1` (URI `2049B`) | `414` | URI 길이 상한 초과 |
| `GET /hello HTTP/2.0` | `505` | 미지원 HTTP 버전 |
| `GET /hello FOO/1.1` | `400` | 잘못된 버전 형식 |

---

## 3. `parse_header_line` & `parse_headers`

### A. `parse_header_line` 문법 분리 (단위 테스트)

| 입력 라인 | 기대 결과 | 검증 포인트 |
| --- | --- | --- |
| `Host: localhost:8080` | 성공, `name="Host"`, `val="localhost:8080"` | 기본 `Key: Value` 분리 |
| `Content-Length:   42 \t` | 성공, `name="Content-Length"`, `val="42"` | value 앞뒤 OWS(`' '`, `'\t'`) 트림 |
| `X-Empty:` | 성공, `name="X-Empty"`, `val=""` | 빈 value 허용 |
| `NoColonHeader` | 실패 (`-1`) | `:` 누락 거부 |
| `: value` | 실패 (`-1`) | 빈 헤더 이름 거부 |
| `Bad Name: value` / `Content-Length : 5` | 실패 (`-1`) | 헤더 이름 내 공백 및 콜론 앞 공백 거부 |
| `Bad@Name: value` / `X-Test: a\x07b` / `X-Test: \xff` | 실패 (`-1`) | `field-name` `tchar` 위반 및 `field-value` 제어 문자·비-ASCII 거부 |

### B. `parse_headers` 의미 해석 및 프레이밍 검증

| 헤더 입력 | 기대 결과 | 검증 포인트 |
| --- | --- | --- |
| `content-length: 10` (소문자) | 성공, `content_length == 10` | 헤더 이름 대소문자 무시 |
| `Content-Length: 0` | 성공, `content_length == 0` | 본문 길이 0 허용 |
| `Content-Length: 1048576` (1 MiB) | 성공 | 본문 최대 상한 경계값 |
| `Content-Length: 1048577` | 실패 (`413`) | 본문 최대 상한 초과 |
| `Content-Length: -1` / `12abc` / `""` | 실패 (`400`) | 잘못된 정수 형식 거부 |
| `Content-Length: 99999999999999999999999` | 실패 (`400`) | `strtoull` / `size_t` 오버플로우 거부 |
| 중복 `Content-Length: 5` + `Content-Length: 5` / `5` + `10` | 실패 (`400`) | 동일 값 포함 중복 `Content-Length` 거부 |
| `Transfer-Encoding: chunked` (단독) / 빈 `Transfer-Encoding:` | 실패 (`501`) / 실패 (`400`) | 미지원 인코딩 거부 및 빈 값 문법 거부 |
| `Transfer-Encoding: chunked` + `Content-Length: 5` | 실패 (`400`) | 모호한 프레이밍 조합 거부 |
| `HTTP/1.1` 요청에서 `Host` 누락 / `HTTP/1.0`에서 `Host` 누락 | 실패 (`400`) / 성공 (`0`) | HTTP/1.1 `Host` 필수 규격 (RFC 9112) |
| 중복 `Host: a` + `Host: b` / 빈 `Host:` / `260B` 초과 `Host` | 실패 (`400`) | 중복·빈·상한 초과 `Host` 거부 |
| 중복 `Content-Type` / `256B` 초과 `Content-Type` | 실패 (`400`) | 중복·상한 초과 `Content-Type` 거부 |
| 헤더 블록 내 `\0` 및 제어 문자(`0x01`, `0x0B`, `0x0C`, `0x1B`, `0x7F`), 비-ASCII(`0x80`~`0xFF`) 포함 | 실패 (`400`) | `validate_head_bytes()` 진입부 일괄 차단 검증 |
| 줄 내부에 단독 `\n` 또는 `\r` 포함 (`GET /hello HTTP/1.1\nHost: a\r\n\r\n`) | 실패 (`400`) | 단독 LF/CR 거부 (Postel's Law 비적용) |

---

## 4. `recv_body(fd, excess, excess_len, content_length, &out_body, &out_len)`

| 시나리오 | 기대 결과 | 검증 포인트 |
| --- | --- | --- |
| `content_length == 0` | 성공 (`0`), `out_body == NULL`, `out_len == 0` | 불필요한 할당 없음 |
| `excess`에 본문 전체가 이미 도착 (`excess_len >= content_length`) | 성공 (`0`), 추가 `recv` 없이 복사 | 헤더와 본문 동반 도착 처리 |
| `excess` 일부(3B) + 추가 `recv` 2회(4B + 3B) | 성공 (`0`), `out_len == 10` | 분할 수신 조합 |
| 중간에 `\0`이 포함된 16바이트 바이너리 본문 | 성공 (`0`), `memcmp` 100% 일치 | `\0` 비의존 바이너리 안전성 |
| 본문 5바이트 중 2바이트 수신 후 조기 `EOF` | 실패 (`-1`), 누수 없음 | 오류 경로 `free` 검증 (ASan) |

---

## 5. `dispatch_request(req, resp)` (소켓 비의존 단위 테스트)

| 입력 `HttpRequest` | 기대 `HttpResponse` | 검증 포인트 |
| --- | --- | --- |
| `GET /hello` / `GET /hello?a=1` | `200 OK`, `text/plain`, `"Hello"` | 기본 GET 및 query string 분리 라우팅 |
| `GET /` | `200 OK`, `text/plain`, `"Hello"` | Iteration 1 회귀 호환 |
| `POST /echo` + `Content-Length: 7` + 본문 `"abc\0def"` | `200 OK`, 본문 `"abc\0def"` (`7B`) | 바이너리 안전 에코 |
| `POST /echo` + `Content-Length: 0` | `200 OK`, `Content-Length: 0`, 빈 본문 | 빈 본문 POST 허용 |
| `POST /echo` + `Content-Length` 누락 | `411 Length Required` | `POST` 길이 헤더 필수 검증 |
| `POST /echo` + `Content-Type: application/json` | `200 OK`, `Content-Type: application/json` | 요청 `Content-Type` 유지 |
| `GET /unknown` | `404 Not Found`, `"404 Not Found\n"` | 미존재 라우트 처리 |
| `GET /echo` / `POST /hello` / `PUT /echo` | `405 Method Not Allowed`, `"405 Method Not Allowed\n"` | 메서드 불일치 및 미지원 메서드 처리 |

---

## 6. `send_http_response(fd, resp)` & E2E 통합 검증 (`make verify`)

| 시나리오 | 기대 결과 | 검증 포인트 |
| --- | --- | --- |
| `curl -sS -i http://127.0.0.1:<port>/hello` | `200 OK`, `Content-Length: 5`, `Connection: close`, `Hello` | GET `/hello` E2E |
| `curl -sS -i -X POST -d "ping" http://127.0.0.1:<port>/echo` | `200 OK`, `Content-Length: 4`, `Connection: close`, `ping` | POST `/echo` E2E |
| 분할 전송 (`POST /echo` 헤더와 본문을 여러 조각으로 나눠 `send`) | `200 OK` + 전체 본문 일치 | TCP 스트림 분할 수신 E2E |
| 잘못된 요청 / 미지원 메서드 / `chunked` / `1MB` 초과 본문 | 각각 `400` / `405` / `501` / `413` 반환 후 서버 생존 | 오류 응답 및 연속 처리 |
| Iteration 1 `verify_iter1.py` 전체 시나리오 | 모두 통과 | 회귀 테스트 통과 |
