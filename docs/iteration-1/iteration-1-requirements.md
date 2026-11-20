# Iteration 1 요구사항 및 구현 경계

기준 문서: [Issue #1](https://github.com/chan95821/jungle-week-13-c-web-server/issues/1), [DEVELOPMENT_CONTEXT.md](../DEVELOPMENT_CONTEXT.md), [web-server.md](../requirements/web-server.md)

---

## 1. 목표

클라이언트가 지정 포트로 TCP 연결을 맺고 최소 HTTP 요청을 보낸 뒤, 유효한 고정 HTTP/1.1 응답을 받는다.

---

## 2. 필수 구현 요구사항 (In-Scope)

### A. 빌드 및 CLI 인터페이스
- `Makefile`로 빌드되어 실행 파일 `./server`를 생성한다.
- POSIX 표준 C API만 사용하여 macOS / Linux / WSL2 이식성을 유지한다. (외부 HTTP·이벤트 루프 라이브러리 금지)
- `./server <port>` 형태로 포트 인자를 받는다.
- 인자 누락, 숫자가 아닌 입력, 유효 포트 범위(`1`~`65535`) 위반 시 오류 메시지를 출력하고 0이 아닌 종료 코드로 종료한다.

### B. TCP 서버 소켓
- `socket()` → `setsockopt(SO_REUSEADDR)` → `bind()` → `listen()` → `accept()` 흐름을 구현한다.
- `bind()` 실패(예: 이미 사용 중인 포트) 시 오류를 출력하고 0이 아닌 종료 코드로 종료한다.

### C. 최소 요청 수신 (프레이밍)
- `connfd`에서 `recv()` 루프로 바이트를 읽어 헤더 종료(`\r\n\r\n`)까지 수신한다. (`EINTR` 시 재시도)
- 버퍼 상한 초과, `\r\n\r\n` 도달 전 조기 `EOF(0)`, 수신 오류 시 응답을 보내지 않고 안전하게 `connfd`를 닫은 뒤 다음 연결을 대기한다.

### D. 고정 HTTP/1.1 응답 송신
- 아래 고정 응답을 반환한다:
  ```http
  HTTP/1.1 200 OK\r\n
  Content-Type: text/plain\r\n
  Content-Length: 5\r\n
  Connection: close\r\n
  \r\n
  Hello
  ```
- C 문자열(`\0`) 기준이 아닌 **명시적 바이트 길이(`size_t`)** 기준으로 송신한다.
- `send()` 부분 송신(short count) 및 `EINTR`을 루프로 끝까지 처리한다.

### E. 자원 수명 및 시그널 안전성
- 단일 연결 순차 처리를 수행한다 (`accept` 1건 처리 후 `close(connfd)`).
- 요청/연결 상태를 전역 공유 버퍼에 두지 않는다.
- 모든 정상·오류 경로에서 `connfd` 소유권과 `close()` 지점을 명확히 유지한다.
- `SIGPIPE`를 무시해 송신 중 클라이언트가 연결을 끊어도 서버 프로세스가 종료되지 않도록 한다.
- `SIGINT` / `SIGTERM` 수신 시 `accept()` 루프를 빠져나와 `listenfd`를 닫고 안전하게 종료한다.

---

## 3. 구현 경계 / 비범위 (Out-of-Scope)

Iteration 1에서는 아래 항목을 구현하지 않는다:
- 요청 라인(`Method`, `URI`, `Version`) 및 헤더(`Host`, `Content-Length`, `Connection`) 파싱 *(→ Iteration 2)*
- `GET` / `POST` 메서드 분기 및 요청 본문(Body) 수신 *(→ Iteration 2)*
- `Connection: keep-alive` (persistent connection) *(→ 이후 Iteration)*
- 정적 파일 서빙, MIME 타입 매핑, `404`/`403`/`500` 동적 생성 *(→ Iteration 3)*
- Reverse Proxy *(→ Iteration 4; Forward Proxy 및 HTTPS `CONNECT`는 전체 범위 제외)*
- 동시 요청 처리 (`fork`, `pthread`, `select`/`poll`/`epoll`/`kqueue`) *(→ Iteration 5)*

---

## 4. 완료 조건 (Definition of Done) 및 반례 기준

- `make` 빌드 성공 및 `./server <port>` 실행
- `curl -i http://127.0.0.1:<port>/` → `HTTP/1.1 200 OK`, `Content-Type: text/plain`, `Content-Length: 5`, `Connection: close`, 본문 `Hello` 확인
- 반복 연결 후 매번 정상 응답 및 FD 누수 없음
- 반례 검증 기준:
  1. 요청 패킷 분할 전송 시 정상 응답
  2. `\r\n\r\n` 완료 전 클라이언트 조기 종료(`EOF`) 시 서버 유지 및 `connfd` 회수
  3. 응답 송신 중 클라이언트 조기 종료 시 `SIGPIPE`로 서버가 죽지 않음
  4. 이미 사용 중인 포트로 서버 중복 실행 시 `bind` 실패 종료
  5. `SIGINT` 수신 시 안전 종료 및 포트 해제
