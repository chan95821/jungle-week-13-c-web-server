# Iteration 5 개인 요구사항 및 설계 경계 (C안: Blocking 스레드 풀)

- **브랜치**: `team/chan95821/iteration-5-thread-pool` (기준선 `main` `577a554`)
- **기준 이슈**: [Issue #5 — Iteration 5 — 동시 요청 처리 (20%)](https://github.com/chan95821/jungle-week-13-c-web-server/issues/5)
- **참고 문서**: [docs/requirements/web-server.md](../../requirements/web-server.md), [docs/DEVELOPMENT_CONTEXT.md](../../DEVELOPMENT_CONTEXT.md), [AGENTS.md](../../../AGENTS.md)
- **상태**: 구현 및 기능·동시성·정적/동적/TSan 검증 완료 (성능 수치 최신화 진행)

---

## 1. 동시성 모델 선정 및 근거

### 선택 모델: C안 (연결 수락 담당 + 제한된 작업 큐 + 고정 Blocking 스레드 풀)

팀 내 세 후보(A: 고정 프로세스 풀 + 프로세스별 `epoll`, B: 고정 스레드 + 스레드별 `epoll`, C: 수락 담당 + 제한된 큐 + blocking 스레드 풀) 중 **C안**을 구현한다.

- **선정 목적**: `epoll` 기반 이벤트 루프 모델(A·B)과의 **구현 비용·단순성·처리량·지연·오류율 비교를 위한 기준 모델(대조군)** 구축.
- **장점**: 기존 순차 요청 처리(`handle_client()` 및 정적·동적·CGI·Reverse Proxy 모듈)를 요청별 독립 데이터 경계 안에서 재사용할 수 있어 코드 변경과 상태 머신 복잡도가 낮다.
- **알려진 한계**: worker 스레드 1개가 한 번에 클라이언트 연결 1개를 전담하므로, 느린 클라이언트·백엔드 요청이 worker 수만큼 동시에 점유되면 이후 요청은 작업 큐에서 대기한다.

---

## 2. In-Scope (이번 작업 범위)

1. **수락 담당 + 제한된 작업 큐 + 고정 스레드 풀**
   - `main()`이 수락 담당으로서 연결을 수락하고 `(client_fd, accepted_at)`을 사용자 공간 링 버퍼 큐(고정 상한 `128`)에 전달한다.
   - 고정 개수(`1..64`, 기본값 `4`)의 worker 스레드가 `pthread_cond_wait()`로 대기하다 작업을 꺼내 `handle_client()`를 실행하고 처리 완료 후 `client_fd`를 닫는다.
   - CLI 인자: 기존 `./build/server <port> <proxy.conf>` 2-인자 계약을 유지하면서 선택적 3번째 인자 `[workers]`를 허용한다.

2. **만석 정책 (방안 1-A: 순수 배압형)**
   - 큐가 가득 차면 수락 스레드는 빈 자리가 날 때까지 대기한다. 초과 연결은 커널 `listen(..., LISTEN_BACKLOG)` 대기열에 머문다.
   - 만석 대기 중 종료 시그널을 놓치지 않도록 `not_full` 조건 변수는 `CLOCK_MONOTONIC` 기반 `pthread_cond_timedwait`(상한 `100ms`)로 대기하며 종료 여부를 재확인한다.
   - 큐 엔트리에 `accepted_at`을 함께 넘겨 기존 `HEADER_DEADLINE_SECONDS`(7초) 계약을 유지한다.

3. **종료 및 시그널 처리 (`self-pipe` + `poll()`)**
   - `main()`이 자기 자신과 연결된 파이프(`stop_pipe[2]`, `O_NONBLOCK` + `FD_CLOEXEC`)를 생성한다.
   - `on_shutdown_signal()`은 `g_running = 0` 설정 후 `write()`로 `stop_pipe[1]`에 1바이트를 기록한다(async-signal-safe).
   - `main()` 수락 루프는 `poll()`로 `listener_fd`와 `stop_pipe[0]`(고정 2개 FD, O(1))을 함께 감시해 `SIGINT`/`SIGTERM`이 어느 스레드로 전달되든 즉시 루프를 탈출한다.
   - 종료 시 큐에 남아 아직 시작되지 않은 `client_fd`는 즉시 `close()`하고, 이미 worker가 처리 중인 연결은 `g_running == 0`을 확인해 정리할 때까지 `pthread_join()`으로 대기한다.

4. **공유 상태 동기화 (`struct proxy_selection_state`)**
   - 조회 백엔드(`PROXY_ROUTE_QUERY`) 라운드 로빈 순번 `next_query_backend`를 전체 worker가 공유한다.
   - `struct proxy_selection_state`에 `pthread_mutex_t`를 두고, `proxy_select_backend()` 내부에서 `PROXY_ROUTE_QUERY`의 순번 읽기·갱신 구간만 lock/unlock으로 보호한다(백엔드 연결·I/O 전 즉시 해제).
   - 초기화·해제 함수로 `mutex` 수명을 관리한다.

5. **멀티스레드 안전 보완 (`src/handlers/cgi.c`)**
   - `spawn_cgi_process()`에서 `pipe(pipefd)` 직후 `set_close_on_exec()`를 `pipefd[0]`, `pipefd[1]` 모두에 적용해 다른 worker의 동시 `fork()` 자식이 파이프 FD를 상속하지 않도록 차단한다(`dup2`로 복제되는 `STDOUT_FILENO`에는 `FD_CLOEXEC`가 상속되지 않음).

6. **전 경로 동시 처리 및 기존 회귀 유지**
   - 정적 파일(`/cafe`, `/menu`, `/assets/...`), server-side 동적(`/greet`), CGI(`/cgi-bin/order`), Reverse Proxy(`/api/submissions`, `/api/submission-status`, `/api/hello`) 모두 동시 요청에서 정상 동작하고, 기존 Iteration 1~4 회귀 테스트를 그대로 통과한다.

---

## 3. Out-of-Scope (제외 범위)

- `epoll` / `kqueue` 기반 비동기 이벤트 루프 및 연결별 I/O 상태 머신 (A·B안 및 후속 비교 대상)
- HTTP `Connection: keep-alive` 지속 연결 및 백엔드 연결 풀링
- 큐 만석 시 `503 Service Unavailable` 조기 거절(load shedding)
- worker 수 동적 증감(오토스케일링) 또는 백엔드 장애 자동 우회
- Forward Proxy 및 HTTPS `CONNECT` 터널링

---

## 4. 탑다운 책임 경계 및 FD 소유권

### 모듈별 책임 경계
- `src/main.c`: CLI 파싱, `proxy_config`/`proxy_selection_state` 초기화·해제, `stop_pipe` + `setup_signals()`, `open_listenfd()`, 스레드 풀 기동·수락 제출·종료 조율.
- `src/thread_pool.h`, `src/thread_pool.c` *(신규)*: 고정 링 버퍼 작업 큐, `mutex`/`cond` 동기화, worker 스레드 수명(`init`/`submit`/`shutdown`) 관리. HTTP·프록시 규칙을 알지 못한다.
- `src/socket_io.h`, `src/socket_io.c`, `src/proxy_internal.h`, `src/proxy_io.c`, `src/proxy_headers.c`, `src/proxy_transfer.c`: `struct cancel_signal { const atomic_int *running; int stop_fd; }`를 통해 종료 플래그와 `stop_pipe[0]`을 하나로 묶어 전달하고, worker 스레드의 대기 중 소켓 I/O도 종료 이벤트를 즉시 감지(`0.00s` 종료)한다.
- `src/proxy.h`, `src/proxy.c`: `proxy_selection_state`의 `mutex` 초기화/해제 및 `proxy_select_backend()` 내 라운드 로빈 임계 구역 보호, `proxy_prepare_response()`에 `const struct cancel_signal *` 전달.
- `src/server.h`, `src/server.c`: `handle_client()`가 `const struct cancel_signal *`를 받아 요청별 스택/힙 자원을 처리·정리하고, `client_fd` 닫기는 호출자(worker)가 담당하도록 계약 주석을 갱신한다.
- `src/handlers/cgi.c`: `spawn_cgi_process()`의 파이프 `FD_CLOEXEC` 보완 외 기존 로직 유지.

### `client_fd` 소유권 규칙
```text
accept() 성공 ─▶ main() 소유
  ├─ thread_pool_submit() 실패 (종료 중) ─▶ main()이 즉시 close(client_fd)
  └─ thread_pool_submit() 성공 ───────────▶ 작업 큐 소유
       ├─ 종료 시 큐에 잔류 ──────────────▶ thread_pool_shutdown()이 close(client_fd)
       └─ worker가 dequeue ───────────────▶ worker 소유 → handle_client() → close(client_fd)
```

---

## 5. 자원 상한 및 메모리/FD 산출 근거

코드베이스 상수 기준 1 요청당 최대 메모리 및 FD 상한은 다음과 같다.

- **활성 요청 1건 최대 메모리**: 스택 피크 `~36 KiB` (`request_header[8192]` + `struct proxy_response_headers` `~24.3 KiB`) + 힙 피크 `~113 KiB` (`request_body` `64 KiB` + `struct proxy_stream` `~16.2 KiB` + 프록시 헤더/필드/트레일러 버퍼) ≈ **약 150 KiB** (worker `4`개 시 활성 요청 ~600 KiB, `64`개 시 ~9.6 MiB).
- **큐 엔트리 1건 메모리**: `{ int client_fd; struct timespec accepted_at; }` = **24 B** (`128`개 기준 `3 KiB`).
- **최대 동시 FD 추정**: `stdio(3) + listener(1) + stop_pipe(2) + queue(128) + workers * 2` → 기본 `4` worker 시 최대 `142`개, 최대 `64` worker 시 `262`개 수준으로 일반 `RLIMIT_NOFILE`(`1024`) 및 현재 환경(`1,048,576`) 내에서 안전하다.
