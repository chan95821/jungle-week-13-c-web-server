# Iteration 5 검증 추적표 및 데모 발표 요약 (C안: Blocking 스레드 풀)

이 문서는 **"AI로 빠르게 구현한 코드가 의도한 설계·제약·성능 목표대로 작성되었음을 어떻게 검증했는가"**를 데모 발표에서 바로 설명할 수 있도록 정리한 요약서다.

---

## 1. "원하는 대로 작성되었다"의 4가지 검증 기준

1. **기능 동작**: 기존 HTTP·정적·동적·CGI·Reverse Proxy 계약과 에지 케이스가 깨지지 않았는가?
2. **구조 일치**: 연결마다 스레드를 새로 만들지 않고, 의도한 **C안(수락 담당 + 제한된 큐 + 고정 blocking 스레드 풀 + 최소 lock)** 그대로 작성됐는가?
3. **안전·자원**: 메모리 누수, FD 누수, 멀티스레드 데이터 경쟁이 없는가?
4. **목적 달성**: 느린 요청 중 정상 요청이 막히지 않는지(HoL 완화)와, worker 고갈 시 대기하는 C안 한계를 정량 수치로 보일 수 있는가?

---

## 2. 의도 ↔ 코드 ↔ 검증 1:1 추적표 (Traceability Matrix)

| 의도 / 요구사항 | 구현 코드 위치 | 검증 테스트 및 도구 | 확인 결과 |
| --- | --- | --- | --- |
| 고정 크기 worker 풀 + 상한 `128` 링 버퍼 큐 (연결당 스레드 생성 금지) | [thread_pool.h](../../../src/thread_pool.h), [thread_pool.c](../../../src/thread_pool.c) | [test_thread_pool.c](../../../tests/unit/test_thread_pool.c), [verify_iter5.py](../../../scripts/verify_iter5.py) (`K == W` 반례) | `workers=2`에서 2개 점유 시 3번째 요청이 대기함 확인 (`[PASS]`) |
| 큐 만석 시 `CLOCK_MONOTONIC` `cond_timedwait(100ms)`로 종료 데드락 방지 | [thread_pool.c](../../../src/thread_pool.c) (`wait_until_not_full`) | [test_thread_pool.c](../../../tests/unit/test_thread_pool.c) (`test_backpressure_running_flag_escape`, `shutdown_wakeup`) | 만석 상태 종료 플래그 시 ≤250ms 내 탈출 & 미제출 FD 호출자 정리 (`[PASS]`) |
| `self-pipe + poll()` 및 워커 I/O 즉시 취소 (`<= 0.75s` 종료) | [main.c](../../../src/main.c), [socket_io.c](../../../src/socket_io.c), [proxy_io.c](../../../src/proxy_io.c) (`struct cancel_signal`) | [verify_edge_cases.py](../../../scripts/verify_edge_cases.py) (`C13`), [verify_iter5.py](../../../scripts/verify_iter5.py) | 유휴·백엔드 대기·큐 적체 중 `SIGTERM` 모두 `<= 0.75s` 종료 (`[PASS]`) |
| 큐 대기 시간을 포함한 `accepted_at` 7초 헤더 타임아웃 유지 | [main.c](../../../src/main.c), [server.c](../../../src/server.c) | [verify_iter5.py](../../../scripts/verify_iter5.py) (`check_queued_deadline_408`) | `workers=1`에서 7.2s 큐 대기 후 `408` 반환 확인 (`[PASS]`) |
| 전역 조회 라운드 로빈(`query-a`/`query-b`) mutex 보호 (I/O 전 즉시 해제) | [proxy.h](../../../src/proxy.h), [proxy.c](../../../src/proxy.c) (`proxy_select_backend`) | [test_proxy_selection.c](../../../tests/unit/test_proxy_selection.c) (8스레드×1000회), [verify_iter5.py](../../../scripts/verify_iter5.py), `Exp 1/3` | 4,000:4,000 / 20:20 / 117,446:117,444 정확 분배 (`[PASS]`) |
| 멀티스레드 CGI `pipe()` + `fork()` FD 상속 차단 및 동시 정상/오류 경로 처리 | [cgi.c](../../../src/handlers/cgi.c) (`set_close_on_exec`), [server.c](../../../src/server.c) | [verify_iter3_dynamic.py](../../../scripts/verify_iter3_dynamic.py), [verify_iter5.py](../../../scripts/verify_iter5.py) | 정상 120건 + 오류(`400/404/502/504`) 40건 동시 부하 통과 (`[PASS]`) |
| 메모리·UB·데이터 경쟁·FD 누수 0건 | 전 모듈 (`atomic_int g_running` 포함) | `make test` (ASan/UBSan 9/9), TSan(`server_tsan` + `test_thread_pool` + `test_proxy_selection`), `/proc/<pid>/fd` | ASan/UBSan 9/9, TSan 경고 0건, 정상·오류 부하 전후 FD `baseline` 일치 |
| 기존 HTTP/프록시 기능 회귀 0건 | [server.c](../../../src/server.c), [http.c](../../../src/http.c), [router.c](../../../src/router.c), `proxy_*` | `make verify` (`verify_iter1..4`, `verify_edge_cases` 40/40) | 전체 회귀 통과 |
| HoL 완화 이득 & `K >= W` 한계 정량 입증 | [iteration-5-performance-results.md](iteration-5-performance-results.md) | [benchmark_iter5.py](../../../scripts/benchmark_iter5.py) (`Exp 1~3`) | `S_MAX` `5,404 → 15,749 req/s` (`w=4`), `K=2 < W=4`에서 제출 `p50` `0.77ms` 유지 |

---

## 3. 검증 과정에서 실제로 잡아내고 고친 결함 3건

발표 시 **"테스트를 돌렸더니 실제로 버그가 잡혔고, 이렇게 고쳤다"**를 강조할 수 있는 실전 사례다.

1. **`C13 SIGTERM` 0.75초 초과 실패 발견 → 워커 I/O 취소 전파 추가**:
   - *문제*: 초기 구현에서는 `main()`만 `stop_pipe`를 보고, worker가 `recv(backend_fd)`에서 1초 타임아웃에 묶여 `SIGTERM`이 1초 걸렸다.
   - *수정*: `struct cancel_signal { const atomic_int *running; int stop_fd; }`로 `running`과 `stop_pipe[0]`을 묶어 전파해 모든 worker 소켓 I/O도 즉시 깨어나도록 수정.
2. **`E17 incomplete body idle` 타임아웃 회귀 발견 → 호출 타임아웃 계약 복구**:
   - *문제*: 위 `wait_socket_or_stop()` 추가 시 5초 호출 타임아웃(`IO_CALL_TIMEOUT_SECONDS`) 만료를 재시도(`continue`)해 15초까지 지연되었다.
   - *수정*: `poll()` 타임아웃 시 기존 `SO_RCVTIMEO`와 동일하게 `ETIMEDOUT`을 반환하도록 수정해 `E17` 복구.
3. **서버 TSan(`-fsanitize=thread`) 검증으로 `g_running` 데이터 경쟁 발견 → C11 `atomic_int` 전환**:
   - *문제*: `volatile sig_atomic_t g_running`은 시그널 안전은 보장하지만 스레드 간 동기화는 보장하지 않아, 통합 서버 TSan 실행에서 `on_shutdown_signal()` ↔ `server_is_running()` 간 데이터 경쟁 경고가 발생했다.
   - *수정*: `_Static_assert(ATOMIC_INT_LOCK_FREE == 2)`로 lock-free가 보장된 C11 `atomic_int`로 교체해 `TSAN_SERVER_RC: 0`, 경고 0건 달성.

---

## 4. 데모 발표 실행 순서 (브라우저 그래픽 시연)

1. **백엔드 3대 + 프록시 기동 (터미널 1회 실행 후 브라우저로 전환)**:
   ```sh
   cat <<'EOF' > /tmp/proxy.conf
   submission 127.0.0.1:8001
   query-a 127.0.0.1:8002
   query-b 127.0.0.1:8003
   EOF
   ./build/test_backend 8001 submission &
   ./build/test_backend 8002 query-a &
   ./build/test_backend 8003 query-b &
   ./build/server 8080 /tmp/proxy.conf 4
   ```
2. **그래픽 대시보드 시연 (`http://127.0.0.1:8080/assets/demo.html`)**:
   - **Reverse Proxy & 라운드 로빈**: `조회 10회 연속` 버튼으로 `query-a` ↔ `query-b` 50:50 분배 바와 노드 펄스 애니메이션, `X-Backend-ID` / `Via: 1.1 jungle-proxy` 로그 실시간 확인.
   - **제출 중계**: `POST /api/submissions` 버튼으로 `submission` 노드(`201`) 응답 확인.
   - **동시성 HoL 완화 Waterfall**: `query-a`를 `delayed-header`로 띄운 상태에서 `동시 요청 6건 발사` 클릭 → 느린 `504`(1초) 막대가 도는 동안 나머지 요청이 수 ms 만에 완료되는 타임라인 시각화.
   - **검증·부하 요약 카드**: 화면 하단에서 4층 기준 배지 및 `Exp 1`/`Exp 2` 비교 막대 그래프 확인.
3. **정적·동적·CGI 데모 페이지**: 대시보드 상단 링크(`/cafe`, `/menu`, `/greet`, `/cgi-bin/order`)로 기존 웹 데모 함께 시연.
4. **자동 검증 증거 (`make test && make verify`)**: 발표 전·후 전체 회귀 통과 상태 제시.
