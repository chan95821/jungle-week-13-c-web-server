# 부모 제어·회수 재시도 깨우기 — 구현 전 선택

2026-11-25. 감독 구현의 다음 연결 단위다. 아직 선택/코드 변경/실행하지 않았다. 기존 공유 mmap/robust mutex·작업자 직접 선택·긴급 pipe·LT epoll은 유지한다.

## 해결할 경합

죽은 작업자의 backend 예약을 회수하려는데 다른 작업자가 mutex를 보유하면 trylock은 EBUSY다. 부모는 다른 I/O를 계속해야 하지만, unlock만으로 부모 epoll이 자동으로 깨어나는 계약은 없다. 외부 요청이 더 없으면 회수가 잠들어 남을 수 있다. 정상 I/O가 계속 ready라고 매 반복 trylock하면 과도한 재시도도 가능하다.

또한 waitpid(WNOHANG)의 EINTR 뒤에는 이미 전달된 SIGCHLD를 다시 기다린다고 회수가 보장되지 않는다. 현재 cleanup_pending은 이 재확인 필요도 포함하므로 타이머 선택은 EBUSY와 waitpid EINTR 재확인을 함께 다룬다. EINTR를 사망 확인으로 취급하지 않는 원칙은 유지한다.

## 1 추천 — pending 때만 임시 10ms 제어 재시도

- EBUSY 또는 waitpid EINTR를 관측한 worker별로 다음 시도를 CLOCK_MONOTONIC 기준 now+10ms에 예약한다. 숫자는 한 곳의 임시 상수로 관리한다.
- 다른 I/O로 일찍 깨어나더라도 해당 worker의 공유 회수 재시도 기한 전에는 trylock을 반복하지 않는다. 새로 사망 확인된 다른 worker와 긴급 종료는 별개로 진행한다.
- parent 다음 epoll 대기는 기존 신호 종료 기한과 가장 가까운 제어 재시도 기한 중 작은 값으로 정한다. HTTP/전달 기한 통합 시에는 그 기한도 함께 고려한다.
- 기한 도달 시 해당 worker당 최대 한 번 비블로킹 재확인한다. 다시 바쁘면 다음 10ms를 예약한다. 완료/전체 fatal로 정상 회수를 중단한 경우 해당 재시도를 해제한다.
- pending이 없으면 주기적 재시도 깨우기를 추가하지 않는다. idle 상태에서 10ms마다 무조건 깨우지 않는다.
- 10ms는 타이머 예약 간격이다. OS 스케줄링/다른 처리/잠금 점유로 실제 완료는 더 늦을 수 있으며 최대 회수 지연이나 성능을 보장하지 않는다.
- 예상 비용: worker별 다음 기한과 pending 종류, pending 동안 시간 기반 wake와 trylock/waitpid 재확인. 정상 unlock 경로에 새 IPC를 넣지 않는다. CPU 비용은 측정하지 않았다.
- 테스트에서 썼던 20ms hang-guard를 운영 정책으로 복사하지 않는다. 이 선택의 10ms는 별도 사용자 선택 대상이다.

## 2 대안 — 잠금 해제 통지 + 재확인 기한

worker가 unlock 뒤 공유 대기 표식을 보고 별도 nonblocking 알림 경로로 부모를 깨운다. unlock 직후 빠르게 회수할 수 있지만 알림 등록/해제·상태 동기화, waiter 설정과 unlock 사이 경합, 통지 전 worker 사망, pipe full/EINTR, 표식 세대와 lost wake를 추가 설계해야 한다. waitpid EINTR와 통지 유실에 대한 fallback 기한도 필요하다. 기존 긴급 pipe를 정상 unlock 알림으로 묵시적으로 재사용하지 않는다. 더 빠르다는 실측 결론은 없다.

## 선택 후 실제 함수·데이터 흐름

main parent loop → advance_parent_worker_supervisor(now) → worker별 reaped/pending 확인 → 기한이 된 회수만 trylock → 다음 제어 기한 기록 → 신호/제어 중 가장 가까운 epoll timeout → wait_process_signal_events → 종료/fatal 우선 확인 → advance.

supervisor 구조에 고정 worker별 기한을 둔다. mutable global·새 process/thread/FD를 추가하지 않는다(1번). 정상 parent accepted/worker/backend 선택 정책과 기존 HTTP 타임아웃은 변경하지 않는다. 이 단위가 HTTP worker 상태 머신이나 전체 main 통합을 완료하는 것은 아니다.

## 검증 후보

- 마지막 외부 I/O 뒤 mutex가 풀린 경우, 추가 요청 없이 timer wake로 회수 진전.
- 다른 I/O가 계속 ready여도 재시도 기한 전 trylock 반복 없음.
- worker별 다른 EBUSY 시각, 기한이 된 worker만 회수, 정상 worker와 종료 신호 진전.
- EINTR로 놓친 waitpid 재확인, 새 SIGCHLD 없이 회수 확인.
- 임시10ms와 종료2000ms의 최소 기한, 0/1ms 경계·기한 지남·단조 시간/오버플로우.
- pending 해제 뒤 불필요한 timer wake 없음, 반복 회수·다른 세대 오염 없음.
- 실제 fork/epoll/robust lock fixture와 계산 단위 검증 구분. 시간 확인은 기능 기한 검증이며 부하/성능 측정은 별도 허락.

## 재시도 1번 채택 (2026-11-25)
worker별 pending 종류(WAIT 재확인/BACKEND 회수)와 다음 기한을 고정 상태에 둔다. EBUSY/백엔드 회수 및 waitpid EINTR 뒤 now+임시10ms를 예약한다. 기한 전 해당 작업만 건너뛰고 다른worker/긴급·종료처리는 계속한다. 다음 epoll 대기는 신호 종료기한과 제어 재시도기한의 최소값이다. pending이 없어지면 주기 wake를 없앤다. 새IPC/FD/정상unlock 통지는 추가하지 않는다. shared fatal의 종료는 재시도기한을 기다리지 않는다.

## 10ms 제어 재시도 완료 (2026-11-25)
사용자1번을 worker별 WAIT/BACKEND retry_kind/retry_at_ms와INITIAL_SUPERVISOR_RETRY_MS=10으로 구현했다. 기한전같은작업보류·due시1회·pending해제·sharedfatal우선, parent_supervisor_wait_timeout_ms로종료/재시도최소기한 제공. 추가요청없는실제epoll타이머회수·ready I/O trylock제한·worker별기한·새CHLD없는EINTR재회수 통과. 일반O2/make all성공, ASan/UBSan전체16개통과. W4감독저장592bytes(이전보다32증가). 상세 iteration-5-supervisor-retry-review.md. main/HTTP worker미통합, E2E/성능미실행. 이전재시도방식미결정문구는이번선택으로대체.
