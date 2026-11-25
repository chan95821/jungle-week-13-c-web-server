# Iteration 5 부하 측정 및 동시성 비교 실험 계획 (C안)

- **브랜치**: `team/chan95821/iteration-5-thread-pool`
- **기준선 비교 대상**: `main` (`577a554`, 순차 처리) vs C안 Blocking 스레드 풀 (`workers = 1, 2, 4, 8, 16, 32`)
- **참고 기존 도구**: [tests/performance/load_client.c](../../../tests/performance/load_client.c), [tests/backends/server.c](../../../tests/backends/server.c), [scripts/benchmark_iter4.py](../../../scripts/benchmark_iter4.py), [Iteration 4 성능 결과](../../iteration-4/Snoopykid/iteration-4-performance-results.md)
- **상태**: 측정 완료 ([iteration-5-performance-results.md](iteration-5-performance-results.md) 참고)

---

## 1. 측정 목적

1. **순차 기준선 대비 오버헤드 확인 (0ms 정상 부하)**:
   - 기존 Iteration 4 정상 조건(`S1`, `S5`, `S_MAX`)에서 `main`(`577a554`) 대비 C안(`workers = 1, 4, 8`)의 처리량과 `p50`/`p95` 지연 차이를 확인한다.
2. **HoL(Head-of-Line) 블로킹 완화 검증**:
   - 일부 요청이 느린 백엔드에 묶여 있을 때(`K < W`), 다른 worker가 빠른 정상 요청을 지연 없이 처리하는지 정량 검증한다.
3. **C안 고유 한계 및 worker 수 민감도 관측 (A·B `epoll` 비교 기준선)**:
   - 느린 요청 수가 worker 수에 도달하거나 초과할 때(`K >= W`) 작업 큐 적체로 지연이 어떻게 변하는지, 그리고 `workers = 1, 2, 4, 8, 16, 32` 변경 시 CPU·RSS·FD·지연 변화를 기록한다.

---

## 2. 도구 재사용 및 격리 원칙

- **부하 생성기**: 기존 C 부하 생성기 [tests/performance/load_client.c](../../../tests/performance/load_client.c) 재사용 (`-O2`, 요청당 새 TCP 연결, `SHUT_WR`, 상태·`Content-Length`·1024B 본문·`X-Backend-ID` 검증).
  - 단, `S_MAX` 포화 실험이나 큐 상한(`128`) 초과 실험처럼 동시 연결 `16`개 이상이 필요한 실험은 복수 `load_client` 프로세스를 병렬 기동하거나 `load_client.c` 상한 확장 여부를 구현·측정 전 별도 확정한다.
- **백엔드**: [tests/backends/server.c](../../../tests/backends/server.c)의 `benchmark` 모드(0ms 1KiB echo)를 기본으로 사용한다.
  - **주의점**: 현재 `tests/backends/server.c`는 단일 스레드 순차 백엔드다. 지연 주입 실험(`K >= 2` 동시 느린 요청)에서 백엔드 자체가 직렬 병목이 되지 않도록, 지연 실험용 백엔드는 독립 지연 포트 복수 배치 또는 동시 지연 응답 지원 여부를 사전 확인한다.
- **증거 보존 및 분리**:
  - 기존 [scripts/benchmark_iter4.py](../../../scripts/benchmark_iter4.py)와 `build/performance/` 산출물은 덮어쓰지 않고 유지한다.
  - Iteration 5용 오케스트레이터(예: `scripts/benchmark_iter5.py`)와 출력 경로(`build/performance-iter5/`)를 분리하고, 비교 대상 스냅샷·도구 SHA256을 `manifest.json`에 기록한다.

---

## 3. 실험 매트릭스

### Exp 1. 정상 0ms 기준선 비교 (`main` vs C안)
Iteration 4와 동일한 조건으로 순차 대비 스레드 풀 오버헤드·확장성을 본다.

| 단계 | 제출(`POST /api/submissions`) | 조회(`GET /api/submission-status`) | 목적 |
| --- | --- | --- | --- |
| `S1` | `2 req/s` (`c=2`) | `10 req/s` (`c=16` 상한) | 저부하 회귀 게이트 (Iteration 4 기준선 `p95` 대조) |
| `S5` | `2 req/s` (`c=2`) | `200 req/s` (`c=16` 상한) | 중부하 지연·오버헤드 비교 |
| `S_MAX` | `2 req/s` (`c=2`) | 무제한 closed-loop (`c=16`) | 포화 처리량(`req/s`) 및 CPU/RSS 비교 |

- **비교 구성**: `direct`(S1/S5), `main`(`577a554`), C안 `workers = 1, 2, 4, 8` (반복 3회, 워밍업 5초 후 본 측정, 실행 순서 교차).

### Exp 2. 혼합 지연 HoL 격리 실험 (핵심 동시성 실험)
느린 백엔드 요청(`delay = 800ms`) `K`개가 동시에 진행되는 동안, 병행하는 빠른 정상 요청(`10 req/s`, 0ms)의 `p50`/`p95` 지연과 완료 순서를 측정한다.

| 조건 | 설정 (`W = 4` 기준 예시) | 기대 관측 |
| --- | --- | --- |
| `K = 0` | 느린 요청 없음 | 정상 기준선 |
| `K = 2` (`K < W`) | 느린 요청 2건 동시 유지 | 남은 2개 worker가 빠른 요청을 `~1-2ms`대에 처리 (순차 서버 대비 HoL 해소) |
| `K = 4` (`K == W`) | 느린 요청 4건 동시 점유 | **C안 한계 지점**: 빠른 요청이 큐에 대기하며 지연이 ~800ms대로 급증 |
| `K = 8` (`K > W`) | 느린 요청 8건 유입 | 큐 적체 및 `workers` 수(`4` vs `8` vs `16`)에 따른 방어 차이 확인 |

### Exp 3. 과부하 · 큐 포화 · 자원 비용 프로파일
- **조건**: `workers = 1, 2, 4, 8, 16, 32` 변화 + 동시 클라이언트 부하 증가(예: `c = 16, 64, 150`으로 큐 상한 `128` 근접/초과).
- **관측 항목**:
  - 처리량(`success_rps`), `p50`/`p95`/`max` 지연, `408`/`5xx`/연결 실패율
  - `VmRSS` 표본 피크 및 `CPU user+system` (worker 스레드 증가에 따른 메모리/컨텍스트 스위치 비용)
  - 전후 열린 FD 수 일치 (`fds_before == fds_after`)

---

## 4. 공통 통제 조건 및 측정 지표

- **빌드 옵션**: `-std=c11 -Wall -Wextra -Werror -O2` (ASan/UBSan 제외).
- **환경 기록**: `uname`, `lscpu`, `nproc`, `free -m`, `RLIMIT_NOFILE`, `ip_local_port_range`, `tcp_tw_reuse`, 측정 전후 `/proc/loadavg`.
- **집계 기준**:
  - 지연: 클라이언트 `socket()` 직전 ~ 응답 EOF·바이트 검증·`close()`까지(`ms`, 성공 응답 기준 반복별 `p50`/`p95` 중앙값 및 min-max).
  - 자원: 프록시/백엔드 `/proc/<pid>/stat` CPU 시간, 100ms 간격 `VmRSS` 피크, 시작/종료 시 `/proc/<pid>/fd` 개수.
  - A/B 분배 균등성: `query-a` / `query-b` 누적 히트 비율(`~50:50`) 확인.

---

## 5. 실행 게이트 (승인 후 수행)

1. Iteration 5 C안 코드 구현 완료
2. `make clean && make all && make test` (ASan/UBSan + TSan 가능 시) 및 `make verify` 전체 통과
3. 위 실험 조건(특히 Exp 2의 지연 백엔드 동시성 구성 및 예상 총 측정 시간)을 사용자에게 보고하고 **명시적 승인**을 받은 뒤 측정 실행
4. 결과 보고서(`docs/iteration-5/chan95821/iteration-5-performance-results.md`)에 A·B안 비교 및 관찰된 한계와 함께 기록
