# Iteration 5 원장 트랜잭션·재복구 실물 코드 리뷰

2026-11-25. 공유 원장 단위 완료. HTTP 서버/작업자 이벤트 루프 통합 완료가 아니다.

## 실제 변경
- src/backend_reservations.c: trylock·복구·불변 조건 검사·작업자 세대·예약/완료·세대 회수·제한 진단.
- src/shared_backend_state.h/c: version2, 고정128-byte undo record의 checksum, shared fatal latch, 시작 시 할당한 process-local 검증 scratch 및 소유권.
- tests/unit/test_backend_reservations.c: test-only checkpoint로 실제 fork/SIGKILL 주입. production 빌드에는 callback/테스트 전역 상태가 없다.
- Makefile: 독립 test_backend_reservations를 make test에 포함. 기존 서버 SRCS/main/HTTP는 미변경.

## main부터의 위치
현재 main은 여전히 기존 순차 서버다. 새 모듈을 실행 경로에 넣었다고 주장하지 않는다.
후속 main: 설정 검증 → create_shared_backend_memory → IPC/listener/fork → 부모 accept/전달·waitpid → worker epoll loop. 아래 공유 상태 흐름을 해당 worker/parent 경계에서 호출한다.

### 1. try_lock_shared_backend_state
pthread_mutex_trylock → EBUSY면 즉시 반환 → EOWNERDEAD면 recover_interrupted_change → 전체 검증 → pthread_mutex_consistent.
잠금 보유 여부는 process-local view에 기록한다. 이 view를 공유 메모리에 넣지 않는다. fork 시 이 값은 false여야 한다.
정상 잠금에서는 header/phase의 한정 검사를 수행하고 매 요청 원장 전체를 스캔하지 않는다. mutex·기록·정합성 실패는 fatal을 latch하고 잠금을 놓아 정상 경로의 재사용을 금지한다. 복구 실패한 inconsistent mutex에는 consistent를 호출하지 않는다.

### 2. activate_backend_worker_locked
전체 원장 정합성 확인 → EMPTY/RETIRED 슬롯만 세대 증가 → ACTIVE. UINT64_MAX 세대는 wrap/reset하지 않고 EOVERFLOW.
이 함수는 슬롯 상태 관리이며 OS 프로세스 생성/자동 재시작이 아니다. 부모의 사망 확인·작업자 생성 정책은 아직 별도.

### 3. reserve_backend_request_locked
호출자가 같은 잠금 아래 이미 선택한 backend와 다음 tie-RR turn을 넘긴다 → worker 세대/빈 예약 슬롯/상한 검사 → prepare_change → 예약 원장 ACTIVE → 작업자 기여 증가 → 전체 수 증가 → 순번 반영 → commit_change → generation token 반환.
이 모듈은 RR/LO/시간/결합 정책을 대신 선택하지 않는다. worker가 선택 후 unlock하고 다시 reserve하면 원자성이 깨지므로 금지. 네 정책의 선택 함수는 나중에 이 동일한 임계 구간에 넣는다. backend는 현재 제출/조회 A/조회 B의 3개 고정 역할 인덱스, turn은 기존 조회2개에 대한0/1이다.
slot 탐색·고갈 때 HTTP 대기/거절과 정책 입력은 아직 미결정. 호출자 제공 slot에 대한 EBUSY/ENOSPC/EOVERFLOW는 내부 결과이며 새 외부 HTTP 정책이 아니다.

### 4. prepare_change와 commit_change
고정 기록은 대상 worker/reservation 인덱스, 이전 worker/예약, 전체 진행 수3개, 이전 순번, checksum을 저장한다. worker lifecycle만 바꿀 때 reservation index UINT32_MAX로 해당 스냅샷을 생략한다.
IDLE: 새 before-image를 채우는 동안 대상 상태는 아직 변경하지 않는다.
PREPARED: seq-cst phase 공개와 fence 뒤 대상 변경. 이 단계에서 종료되면 이전 값으로 복원한다.
COMMITTED: 모든 대상 쓰기 뒤 fence/seq-cst commit. 이 단계 종료는 변경을 보존한다.
IDLE: 기록을 유휴로 전환. 이후 잠금 해제 전 사망해도 상태 검증 후 보존한다.
checksum은 필드별 숫자 hash로 padding에 의존하지 않는다. 우발 기록 손상 검사를 보강할 뿐 악의적 쓰기/충돌/그럴듯하게 일치하는 임의 손상을 방어하지 않는다. 이 프로토콜은 현재 Linux/WSL의 fork/익명 공유 메모리와 lock-free atomic 지원 범위이며 디스크 영속/WAL 보장이 아니다.

### 5. complete_backend_request_locked
token의 worker/request 세대·슬롯·ACTIVE 확인 → 동일 before-image 트랜잭션 → FREE → worker/global 감소 → commit. 정상 해제0, 과거·이미 완료/회수 token1(무변경), 오류-1.
예약 슬롯 재사용에도 request generation은 증가해 이전 완료가 새 예약을 건드리지 않는다. 완료 시점·응답 시간 표본/실패 페널티는 미결정이므로 현재 이 함수에는 시간 통계가 없다.

### 6. recover_interrupted_change
phase·checksum·인덱스 검증 → PREPARED면 동일 before-image로 예약/worker/전체 수/순번 복원 → validate_backend_reservations_locked → IDLE → consistent.
복구 중 원본 기록을 수정하지 않는다. 복원은 +=가 아닌 이전 값 대입이므로 도중 종료 후 반복 적용 가능. COMMITTED/IDLE은 대상 상태를 되돌리지 않고 전체 정합 검사한다. 복구 자체가 어느 단계에서 멈췄든 새로운 획득자가 EOWNERDEAD를 받아 재검사한다. record가 신뢰 불가이면 추정 복구하지 않는다.

### 7. reclaim_backend_worker_locked
부모가 waitpid로 사망을 확인한 후 호출하는 계약이다. 이 함수 안에서는 waitpid/I/O를 하지 않는다.
전체 검증 → 같은 세대 ACTIVE를 RETIRING으로 commit → 그 세대 ACTIVE 예약만 각 complete 트랜잭션으로 해제 → RETIRED commit.
회수자 종료 시 이미 commit된 해제는 유지, 미commit 해제만 rollback. 재호출은 남은 ACTIVE만 해제한다. 새 세대에 대한 과거 회수는 무변경. 살아 있는 작업자의 원장·기여분·순번은 보존한다.
현재 회수 전체 동안 mutex를 보유한다. 전체 원장 상한 안의 O(R+WB+K) CPU 작업이므로 공유 갱신을 지연시킬 수 있다. 네트워크/파일 I/O나 malloc은 하지 않는다. 배치 회수/재개·최대 허용 지연은 성능 결과 없이 자동 도입하지 않았다.

### 8. validate_backend_reservations_locked와 진단
시작 시 할당한 로컬 scratch로 O(R+WB)에 ACTIVE 예약 수를 집계. backend 전체 수=worker 기여 합계=ACTIVE 원장 수, 세대·상태·인덱스·예약 유효성을 확인.
정상 요청은 대상 구조의 한정 검사, lifecycle/복구·명시 검증은 전체 검사다. 아직 건드리지 않은 손상이나 범위 안에서 함께 일치하게 손상된 값은 즉시/항상 탐지 보장하지 않는다. robust는 임의 메모리 손상 감지 기능이 아니다.
실패는 atomic fatal + EUCLEAN + process-local diagnostic으로 반환. format_backend_state_diagnostic은 lock 밖에서 호출하며 숫자 worker/generation/failed_check/recovery_phase/recovery_stage와 고정 종료 이유만 출력, 임의 shared 문자열·본문·URI·헤더를 읽지 않는다. 출력 공간 부족은 ENOSPC.
아직 stderr/log sink/부모 통지·부모 accept 중단·전체 worker 종료와 연결하지 않았다. 따라서 현 상태를 서버 전체 안전 종료 구현 완료로 표현하지 않는다. 다음 parent IPC 흐름에서 반드시 연결한다. 이미 나간 HTTP 응답에 새 오류를 붙이지 않는 기존 정책 유지.

## 메모리·정상 잠금 비용 갱신
현재 WSL sizeof: header232(undo128 포함), worker40, reservation32. 매핑 M=232+40W+32R(현재 정렬).
W4/R10000 예시320392bytes≈312.9KiB. W4/R16384 예시524680bytes≈512.4KiB. 이전 초기화 보고서의 header216/record120 예시는 당시 관측값이며 현재 checksum/fatal 추가로 16bytes 증가했다.
로컬 검증 scratch는 24W bytes/프로세스(W4이면96bytes), process-local view·allocator 관리·페이지 COW는 별도. 공유 매핑은 프로세스 RSS 합산 시 중복 주의. 미결정 시간 통계/연결·IPC/커널 socket/epoll 비용은 미포함.
시작 시 W/R/byte budget 상한을 검증하고 고정한다. 실행 중 원장/journal 확장 없음. 단일 mutex의 동시 journal1개128bytes.
요청 예약/완료 각각 한 임계 구간, 고정 before-image와 phase3회 저장/필요 fence·checksum. chunk마다 잠그지 않음. 선택 알고리즘의 통계·동률 계산 비용은 아직 없음. busy 재시도는 호출자 event loop의 보류 정책이 필요하며 이 모듈은 spin하지 않는다.

## 컴파일 확인
- -std=c11 -Wall -Wextra -Werror -O2 -pthread로 hook 없는 backend_reservations.o 컴파일 성공.
- make의 새2개/기존8개 단위 바이너리 컴파일·링크 확인 성공. 새 단위는 ASan/UBSan, 테스트빌드만 SHARED_BACKEND_FAULT_TEST 사용.
- 최초 compile의 POSIX 선언 매크로 누락은 _POSIX_C_SOURCE=200809L을 파일 첫 줄에 추가해 수정 후 재컴파일했다.

## 실제 실행 결과
make test: 10개 바이너리 통과, failure/skip0. 이후 추가한 commit 보존 재복구6경계를 포함해 test_backend_reservations 재실행 통과. sanitizer 진단 없음.
원장 시험:
- 정상/늦은·중복 완료, worker/request 세대 재사용, request generation 포화 거절.
- reserve/complete 변경 경계8+8.
- generation 변경 경계6(세대 쓰기와 state 쓰기 사이 포함).
- PREPARED rollback 중 복구자 사망7경계.
- COMMITTED/IDLE 검증·consistent 중 재사망6경계(완료 상태 보존).
- 회수의 RETIRING/해제1/해제2/RETIRED 각5경계, 총20.
- 상태 손상6, 복구 기록 손상3: 진단/fatal/새 갱신 금지, ENOTRECOVERABLE 거절.
- parent가 mutex를 보유하고 pipe 장벽으로 child4개가 EBUSY를 확인한 뒤 해제, worker별32쌍 reserve/release. 순위/처리량/지연/CPU 계측 없음.
각 장애 시험은 생존 worker의 예약과 기여를 byte 비교하고 불변 조건·회수 반복 무변경을 검사했다. 실제 TCP backend/epoll 서버 프로세스의 worker를 죽이는 E2E는 아니다.
로그: build/iteration5-capability/unit-tests.log(10개 회귀), reservations-test.log(최종 원장 단위). test alarm60s/harness75s는 시험 hang guard이며 운영 복구 timeout 정책이 아니다.

## 미검증·남은 범위
- 부모/worker 실제 IPC 전달·전체 종료·worker epoll 통합, backend/client FD 회수 E2E.
- mmap/mutexattr/mutex-init 실패 주입, ENOMEM 주입, 32-bit ABI/다른 OS, raw mutex 메모리 임의 손상.
- 끝없는 반복 장애의 복구 시도/시간 제한, 재시작/부모 사망·강제 종료 기한.
- 원장 포화의 HTTP 동작, 실제 슬롯 선택/연결 상한, 대기 요청/완료 보류의 event loop 재개 정책.
- 네 알고리즘·시간 평균/측정구간·실패 반영·설정 선택, 부하/성능 비교.
기존 make verify 서버 E2E는 이번 단위에서 실행하지 않았다. main/기존 서버 실행 경로 미변경. 커밋/머지/푸시 없음.
