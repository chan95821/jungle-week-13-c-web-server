# Iteration 5 테스트 기준 (C안: Blocking 스레드 풀)

이 문서는 Iteration 5 개인 구현(`team/chan95821/iteration-5-thread-pool`)에서 단위·통합·동시성 검증을 통해 확인한 동작과 경계 사례를 정의한다. 실제 테스트 코드(`tests/unit/test_thread_pool.c`, `tests/unit/test_proxy_selection.c`, `scripts/verify_iter5.py`)와 1:1로 대응한다.

---

## 1. 작업 큐 및 스레드 풀 단위 테스트 (`tests/unit/test_thread_pool.c`)

### 초기화·제출·종료 인자 및 상태 경계 (`test_invalid_args_and_immediate_shutdown`)
- `thread_pool_init()`: `pool == NULL`, `task_fn == NULL`, `worker_count == 0`, `queue_capacity == 0` 전달 시 `-1` (`EINVAL`)을 반환하고 스레드·메모리·FD 자원을 남기지 않는다.
- `thread_pool_submit()`: `pool == NULL`, 미초기화(`initialized == false`) 풀, `client_fd < 0`, `accepted_at == NULL` 전달 시 `-1` (`EINVAL`)을 반환한다.
- `thread_pool_shutdown()`: `NULL` 포인터 및 미초기화 풀 호출을 안전하게 무시하며, 초기화된 풀에 대해 `thread_pool_shutdown(&pool)`을 연속 2회 호출(이중 종료 멱등성)해도 안전하다.
- 초기화 직후 `shutdown` 시 메모리·스레드·FD 누수(`/proc/self/fd` 전후 동일) 없이 종료된다(ASan/UBSan 확인).

### 정상 제출·소비, 타임스탬프 보존 및 FD 소유권 (`test_exact_once_and_fd_ownership`)
- 단일 생산자(1×64건)와 다중 생산자(4×32건)가 파이프 읽기 FD를 제출하면 모든 항목이 누락·중복 없이(`seen[id] == 1`) worker에서 정확히 1회씩 처리된다.
- 각 작업의 `accepted_at` (`tv_sec`, `tv_nsec`) 값이 링 버퍼를 거쳐 `task_fn`에 손상 없이 전달된다.
- worker가 작업을 마치면 `client_fd`가 닫히고(`fcntl(fd, F_GETFD) == -1`, `EBADF`), 전체 처리 후 열린 FD 개수가 시작 시점과 일치한다.

### 큐 상한 및 만석 배압·종료 탈출 (`cond_timedwait`)
- `test_backpressure_unblock_on_worker_finish`: `workers=1, queue_capacity=2`에서 worker 1개를 게이트로 블로킹한 뒤 작업 2개를 채우고 4번째 `submit`을 호출하면 빈 자리가 날 때까지 대기하며, 게이트가 열리면 4건 모두 1회씩 완료되고 FD가 닫힌다.
- `test_backpressure_running_flag_escape`: 큐가 가득 차 `cond_timedwait` 대기 중인 상태에서 `SIGUSR1` 핸들러로 `running = 0`이 되면 `<= 250ms` 이내에 `-1` (`ECANCELED`)로 탈출한다. 제출되지 않은 FD는 호출자가 소유한 채 열려 있으며, 게이트 해제·큐 드레인 후에도 `running == 0` 상태에서는 후속 `submit`이 계속 `ECANCELED`로 거부된다.
- `test_backpressure_shutdown_wakeup`: `running == NULL` 구성에서 `submit`이 만석 대기 중일 때 다른 스레드가 `thread_pool_shutdown()`을 호출하면 `<= 250ms` 이내 `ECANCELED`로 깨어나며(미제출 FD는 호출자가 정리), 기실행 작업은 완료되고 미처리 큐 작업은 실행되지 않은 채 FD가 닫힌다.

### 종료 시 잔여 항목 폐기 및 활성 worker 합류 (`test_shutdown_drains_unstarted_fds_and_joins_active_worker`)
- worker가 꺼내기 전 큐에 남아 있던 미처리 작업들은 `shutdown` 호출 시 `task_fn`을 **실행하지 않고(`seen == 0`)** 폐기하며 각 `client_fd`를 모두 닫는다.
- 이미 실행 중인 worker는 현재 작업을 마칠 때까지 `pthread_join`으로 대기한 뒤 종료된다.

---

## 2. 조회 백엔드 선택 동기화 단위 테스트 (`tests/unit/test_proxy_selection.c`)

- `test_selection_lifecycle_null`: `proxy_selection_init(NULL)`은 `-1` (`EINVAL`)을 반환하고, `proxy_selection_destroy(NULL)`은 무오류로 안전하게 반환한다.
- `test_concurrent_query_selection`: `proxy_selection_init()` 이후 8개 스레드가 동시에 `PROXY_ROUTE_QUERY`로 `proxy_select_backend()`를 각 1,000회(총 8,000회) 호출해도 데이터 경쟁(TSan)이 발생하지 않는다.
- 모든 스레드 종료 후 `query_backends[0]`과 `query_backends[1]`의 총 선택 횟수가 정확히 `4,000` : `4,000` (50:50)으로 일치한다.
- 같은 루프 안에서 `PROXY_ROUTE_SUBMISSIONS`를 8,000회 동시 호출해도 `next_query_backend` 순번을 소비하지 않으며 조회 라운드 로빈 균형을 깨지 않는다.
- 기존 `test_proxy_selection` 단일 스레드 동작(설정 파싱, 라우트 테이블, 초기 순서, 독립 상태, 유효하지 않은 route `NULL`, 보호 헤더 정책) 회귀가 모두 통과한다.

---

## 3. CLI 인자 및 서버 기동/종료 E2E 테스트 (`scripts/verify_iter5.py`)

### CLI 하위호환 및 `[workers]` 검증 (`check_cli_workers`)
- `./build/server <port> <proxy.conf>` (2-인자): 기본 worker 수(`4`)로 정상 기동 및 기존 Iteration 1~4 E2E(`make verify`) 전체 통과.
- `./build/server <port> <proxy.conf> <workers>`: `"1"`, `"4"`, `"8"` 지정 시 각각 정상 기동·응답(`200`) 후 `SIGTERM` `<= 0.75s` 종료 코드 `0` 확인.
- 잘못된 worker 수(`"0"`, `"-1"`, `"65"`, `"abc"`, `""`, `"1x"`) 또는 초과 인자(`argc > 4`) 입력 시 리스닝 소켓을 열기 전 `exit != 0`으로 실패 종료.

### 시그널 즉시 종료 (`check_idle_and_active_sigterm`, `check_sigterm_while_queued`, `check_sigterm_during_slow_cgi`)
- 유휴 대기 상태(요청 0건)에서 `SIGINT` / `SIGTERM` 전송 시 `0.75s` 이내 종료 코드 `0`으로 종료.
- 느린 백엔드 응답 대기 중 `SIGTERM` 전송 시(`verify_edge_cases.py` `C13` 포함) `0.75s` 이내 정상 종료 및 열린 FD 미누수.
- `workers=1`에서 작업 1건이 워커를 점유하고 8건이 큐에 대기 중일 때 `SIGTERM` 전송 시 `0.75s` 이내 종료 코드 `0`으로 종료하며, 점유·대기 중이던 클라이언트 소켓 9개 전부 추가 잔여 데이터 없이 EOF/리셋으로 닫힘을 확인한다.
- 느린 CGI 자식이 헤더와 첫 청크를 보낸 뒤 `wait_readable()`에서 추가 출력을 대기하는 도중 `SIGTERM` 전송 시 `stop_fd` 감지로 즉시 자식을 정리·회수하고 `0.75s` 이내 종료 코드 `0`으로 종료함을 확인한다.

### CGI 멀티스레드 안전 (`verify_iter3_dynamic.py`, `verify_iter5.py`)
- 여러 클라이언트가 동시에 `/cgi-bin/order`를 요청할 때 각 응답 본문과 상태가 섞이지 않고 모두 정상 `200` 완료하며 좀비 프로세스가 남지 않는다.
- 한 worker가 CGI 자식 실행 중 다른 worker가 동시에 CGI 또는 프록시 요청을 수행해도 파이프 EOF가 지연되지 않는다(`FD_CLOEXEC`).

---

## 4. 동시성 핵심 검증 (`scripts/verify_iter5.py`, Issue #5 DoD 및 C안 한계)

### 1) 느린 요청과 빠른 요청의 격리 (`check_hol_isolation`, `K < W`)
- 기본 `workers=4` 구성에서 `query-a`/`query-b`를 `delayed-header` 모드(`1s` 백엔드 헤더 타임아웃 → `504`)로 둔 채 느린 프록시 요청 1건을 먼저 보내고, 직후 보낸 빠른 요청(`/cafe`, `/hello`, `POST /api/submissions`)이 느린 요청 완료를 기다리지 않고 `0.4s` 이내 먼저 `200`/`201` 완료됨을 확인한다. (고정 `800ms` 백엔드 지연 대비 정량 격리는 [iteration-5-benchmark-plan.md](iteration-5-benchmark-plan.md) `Exp 2`에서 측정한다.)

### 2) C안 고유 한계 확인 (`check_all_workers_occupied_limit`, `K == W`)
- `workers=2` 구성에서 미완료 `POST /echo` 요청 2건이 두 worker를 모두 점유한 상태에서 3번째 `GET /hello` 요청을 보내면, 앞선 worker 1개가 완료되어 비워질 때까지 3번째 요청이 큐에서 대기(`>= 0.2s` 지연, 선행 차단)한 뒤 `200 Hello`로 정상 응답함을 확인한다.

### 3) 큐 대기 시간 타임아웃 계약 (`check_queued_deadline_408`)
- `workers=1`에서 5초 유휴 타임아웃 미만 간격(`3.5s`, `3.6s`)으로 천천히 본문을 전송하는 `POST /echo` 1건으로 worker를 7.1초 이상 점유한 상태에서 2번째 `GET /hello`를 큐에 넣으면, 선행 요청은 `200` 완주하고 7초(`HEADER_DEADLINE_SECONDS`)를 넘긴 큐 대기 요청은 `accepted_at` 기준으로 **`408 Request Timeout`** 응답을 받는다.

### 4) 정상·오류 경로 동시 부하 및 자원 회수 (`check_concurrent_all_paths_and_fds`)
- `12`개 클라이언트 스레드로 정상 6개 경로(정적 `/cafe`, 동적 `/greet`, CGI `/cgi-bin/order`, 프록시 `POST /api/submissions`, `GET /api/submission-status`, `GET /api/hello`) `120`건을 동시 실행해 전건 성공, `query-a`/`query-b` 정확 `20:20` 분배, `/proc/<pid>/fd` 개수 일치(`baseline == after`)를 확인한다.
- 이어서 `submission=reset`·`query=delayed-header` 상태에서 `12`개 스레드로 오류 4종(`400` `Host` 누락, `404` 없는 경로, `502` 백엔드 리셋, `504` 헤더 타임아웃) `40`건을 동시 실행해 각 기대 상태 코드 일치, `/proc/<pid>/fd` 개수 일치, 정상 백엔드 복구 후 `healthy_query()` 정상 응답을 확인한다.
- ASan/UBSan(`make test` 9종) 및 ThreadSanitizer(`server_tsan` + `test_thread_pool` + `test_proxy_selection`) 오류 0건을 확인한다.

---

## 5. 부하 측정 및 성능 비교

정량 부하 측정(정상 기준선 `S1/S5/S_MAX`, 혼합 `800ms` 지연 HoL 격리, 큐 포화 및 `workers` 수 민감도)은 [iteration-5-benchmark-plan.md](iteration-5-benchmark-plan.md)에 정의하고 측정 결과를 [iteration-5-performance-results.md](iteration-5-performance-results.md)에 기록했다.
