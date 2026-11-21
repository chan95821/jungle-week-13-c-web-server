# jungle-week-13-c-web-server

POSIX 소켓 API와 C로 구현한 순차 처리 HTTP 서버입니다.
현재 범위는 Iteration 2: 요청 파싱, GET/POST, 동적 응답, Content-Length 기반 본문 수신입니다.

## 빌드와 실행

Linux/macOS/WSL2에서 GCC 또는 Clang, make, Python 3, curl을 사용합니다.

```sh
make
./server 8080
```

다른 터미널에서:

```sh
curl -i http://127.0.0.1:8080/
# 기존 응답: Hello

curl -i 'http://127.0.0.1:8080/hello?name=Jungle'
# 동적 응답: Hello, Jungle!

curl -i --data-binary 'hello' http://127.0.0.1:8080/echo
# 본문을 그대로 반환: hello
```

## 코드를 읽는 순서

```text
src/main.c: main
  ├─ parse_port: CLI 포트 검증
  ├─ setup_signals: SIGPIPE 무시, SIGINT/SIGTERM 종료 요청
  ├─ open_listenfd: socket → bind → listen, 연결 입구 생성
  └─ poll → accept → handle_client → close(connfd) 반복

src/server.c: handle_client
  ├─ recv_request_headers: CRLFCRLF까지 누적 수신
  ├─ parse_http_request: 요청 라인/헤더 해석 (src/http.c)
  ├─ recv_request_body: 이미 받은 본문 보존 + 부족한 바이트 수신
  ├─ route_request: 메서드/경로로 응답 결정 (src/http.c)
  ├─ send_response → send_all: 헤더와 본문 전체 전송
  └─ free(request.body): 요청별 메모리 정리
```

`socket`, `bind`, `listen`, `poll`, `accept`, `recv`, `send`, `close`는 POSIX API입니다.
위의 나머지 함수는 이 프로젝트에서 구현했습니다.

### 헤더 길이와 전체 수신 길이를 나누는 이유

TCP는 바이트 스트림이므로 한 번의 recv에 헤더 일부만 올 수도 있고 본문까지 올 수도 있습니다.

```text
첫 recv: [POST /echo ... Content-Length: 5 CRLFCRLF][hel]
          └──────── header_length ────────────────┘
          └──────────────── received ─────────────────┘

이미 받은 본문 = received - header_length = 3
추가로 필요한 본문 = Content-Length - 3 = 2
다음 recv: [lo]
완성된 본문: [hello]
```

`recv_request_headers`는 두 길이를 모두 반환합니다. 파서는 헤더 부분만 보고,
본문 수신 함수는 함께 받은 데이터를 먼저 복사한 뒤 남은 길이만 읽습니다.
본문에는 NUL 바이트도 들어갈 수 있으므로 문자열 검색이나 strlen으로 끝을 판단하지 않습니다.

### 요청과 응답의 소유권

- main이 connfd를 소유하고 handle_client가 성공/실패로 돌아온 뒤 닫습니다.
- handle_client가 요청 본문을 할당하고 공통 정리 지점에서 해제합니다.
- echo 응답은 요청 본문을 빌려 쓰므로 전송 완료 후에 본문을 해제합니다.
- 동적 인사 응답은 response.text를 사용합니다. 응답 구조체 자체는 스택에 있습니다.

## 지원 범위

| 요청 | 결과 |
| --- | --- |
| GET / | 200, 기존 Hello 응답 |
| GET /hello | 200, Hello, World! + 개행 |
| GET /hello?name=Jungle | 200, 입력에 따른 인사 |
| POST /echo | 200, 받은 본문의 바이트를 그대로 반환 |
| 존재하지 않는 경로 | 404 |
| 알려진 경로에 GET/POST 중 허용하지 않는 메서드 | 405 + Allow |

HTTP/1.0과 HTTP/1.1을 받으며, HTTP/1.1은 Host가 필요합니다.
헤더 이름은 대소문자를 구분하지 않습니다. Host는 보관하고 Connection 토큰은 해석하지만,
모든 응답은 Connection: close이며 연결당 요청 하나만 처리합니다.

- 헤더: 마지막 CRLFCRLF 포함 최대 8192바이트, 초과 시 431.
- 요청 대상: 최대 2048바이트, 초과 시 414.
- 본문: 최대 1 MiB, 초과 시 할당 전에 413.
- POST는 Content-Length 필수(없으면 411), 0바이트 본문 허용.
- 중복 Content-Length, 잘못된 길이, Transfer-Encoding과 길이의 동시 지정은 400.
- Transfer-Encoding 단독은 501, Expect는 417로 즉시 거부.
- 각 recv/send 호출의 대기 제한은 5초. 요청 전체 시간 제한은 아닙니다.
- /hello의 name은 최대 256바이트이며 %HH와 +를 디코딩합니다.
  중복 name, 잘못된 인코딩, 제어 문자는 400입니다. 쿼리 키는 문자 그대로 비교합니다.

정적 파일, JSON/form 파싱, chunked 디코딩, keep-alive, pipelining,
동시성 및 Reverse Proxy는 아직 구현하지 않았습니다.

## 검증

```sh
make test          # 기존 3개 + 파서/라우팅 + 본문 수신: ASan/UBSan 단위 테스트
make verify        # 실제 서버를 실행하는 Iteration 2 E2E 17개
make verify-iter1  # 이전 단계의 동작 회귀 검사 (새 HTTP 검증 정책 반영)
make sanitize      # 서버도 sanitizer로 재빌드한 뒤 단위/E2E 실행
```

Iteration 1 검증에서는 HTTP/1.1의 Host와 응답 분할 수신을 올바르게 처리하도록 보완했습니다.
헤더 상한 오류의 기대 결과도 무응답 종료에서 431 응답으로 갱신했습니다.

개인 구현 범위와 테스트 기준은 [요구사항](docs/iteration-2/jaeseong/iteration-2-requirements.md),
[테스트 기록](docs/iteration-2/jaeseong/iteration-2-tests.md)에 정리했습니다.
