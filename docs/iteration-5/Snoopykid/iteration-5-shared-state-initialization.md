# 구조 A 첫 코드 리뷰 — 공유 영역 초기화

2026-11-25. 사용자 승인: ①~④ 복구, 원장·복구 기록 상한, 단계별 장애 주입; ⑤ 탐지된 모순을 제한된 진단으로 기록하고 신뢰 불가 시 전체 안전 종료; 자료구조 A.
이 승인으로 이전 문서의 A/B/C 선택 대기와 복구 불가 대응 미결정 부분을 대체한다. 대체 worker·재시작 제한·알고리즘 통계·실행 상한 수치·부하/성능 승인은 별도다.

## 이번 실제 구현 범위
src/shared_backend_state.h/c: 슬롯별 타입, 크기 계획, 익명 공유 mmap, process-shared robust mutex와 lock-free commit 표시용 atomic 초기화, 부모의 최종 해제.
이번에는 reserve/complete/rollback/recover/진단 로그/부모 통지/epoll loop를 아직 구현하지 않았다. 복구 record는 초기화된 고정 저장 공간이며 실제 commit/recovery 연산이 구현된 것이 아니다. 시간 통계 필드는 정책 미결정이므로 현재 크기에 포함하지 않았다.
main/기존 서버 동작에는 아직 연결하지 않았다. Makefile에는 독립 테스트 타깃과 make test 목록만 추가했다. 제품 src 목록/HTTP 처리/프로덕션 링크 옵션은 유지.

## main부터 연결할 순서와 현재 코드의 관계
향후 main: 설정 검증 → plan_shared_backend_memory → create_shared_backend_memory → listener/IPC/fork → 부모 accept/전달·회수 + worker loop → 모든 공유 사용자 종료 → destroy_shared_backend_memory.
현재 main은 여전히 순차 서버이며 위 호출 추가 전이다. 다음 직접 구현 함수가 현재 단계의 실물 코드다.
1. plan_shared_backend_memory: worker/reservation capacity와 memory_budget 검증 → align/곱셈/덧셈 overflow 검사 → 전체 bytes 계산 → 예산 초과 E2BIG. 매핑 없이 크기만 알 수 있다.
2. create_shared_backend_memory: 빈 process-local owner 검사 → plan 호출 → MAP_SHARED|MAP_ANONYMOUS mmap → commit 표시용 atomic 초기화/lock-free 확인 → initialize_shared_mutex → metadata 설정 → 완성된 owner/view 게시.
3. initialize_shared_mutex: attr_init → setpshared(PROCESS_SHARED) → setrobust(ROBUST) → mutex_init → attr_destroy. 실패 반환값을 errno로 전달, private/non-robust fallback 없음.
4. destroy_shared_backend_memory: 모든 자식/접근이 끝났다는 호출자 전제 → mutex_destroy → process-local owner에 보관한 길이로 munmap → owner 0. 공유 header가 손상한 길이를 unmap에 쓰지 않는다. 파괴 실패 시 소유권 유지. 자식이 살아 있을 때 호출해서 안전하게 EBUSY가 나온다고 보장하지 않는다(잘못된 사용).
공유 매핑 자체는 FD를 소유하지 않는다. 생성 시 fd=-1. listener/client/backend/IPC FD 수명은 다음 parent/worker 흐름 구현 대상.

## 상한과 메모리 예산
- worker_capacity·reservation_capacity·memory_budget를 필수 함수 입력으로 받는다. 임의 실행 기본값을 넣지 않는다. 최종 CLI/설정 입력은 별도 결정.
- 각 capacity는 1~UINT32_MAX, 실제 허용량은 memory_budget 및 size_t overflow 검사로 더 제한. 이 정수 표현 한계는 권장 운영 상한이 아니다.
- 예약 슬롯은 서버 전체의 고정 배열이며 실행 중 확대 없음. 상한 소진 때 대기/거절의 외부 동작은 미결정, 현재 초기화 코드에는 처리 요청 자체가 없다.
- 단일 mutex 아래 동시에 진행되는 변경은 1개이므로 고정 undo record 1개(현재 120 bytes)를 header 안에 배치. 요청 수만큼 journal이 생기지 않는다.
- 현재 WSL ABI: header 216 bytes(120-byte recovery 포함), worker 슬롯 40 bytes(세대+backend3 기여분+상태), 요청 예약 32 bytes(작업자/요청 세대·인덱스·backend·상태). 정렬 후 M=216+40W+32R. 향후 통계·원장 탐색 보조 구조·복구 검증 필드에 따라 version/산식 갱신 필요.
- 예시 W4/R10000: 216+160+320000=320376 bytes, 약312.9KiB. W4/R16384: 524664 bytes, 약512.4KiB. 이 예시는 runtime 설정 선택·동시 연결 처리 보장·성능 측정 아님.
- mmap의 page 단위 반올림과 커널 관리, 프로세스 로컬 view, 연결 header/body/relay buffers·IPC/socket/epoll·통계는 별도. RSS 합산은 공유 페이지 중복 주의.
- 현재 정상 요청 갱신은 미구현. 설계 예산은 예약/선택1회+완료/해제1회 잠금과 영향받은 필드의 before-image 기록. chunk마다 잠그지 않는 원칙 유지. 복구는 예약 상한 내 스캔 비용을 추가 평가한다.

## 승인한 복구 로그 계약과 후속 구현
요청 본문/URI/헤더/토큰 등 민감 입력을 출력하지 않는다. 작업자 슬롯·세대·실패한 검사(enum)·복구 단계(enum)·종료 이유(enum)를 고정 형식·한정된 행 크기로 로컬 진단 스냅샷에 담는다. 공유 문자열/포인터를 %s로 읽거나 원장 전체를 dump하지 않는다. 손상한 값은 untrusted 표시와 범위 검사, 진단도 신뢰 가능한 local capacities를 기준으로 한다.
I/O는 잠금 밖에서 수행하므로 실패 위치의 숫자/enum을 먼저 로컬로 복사하고 잠금 상태를 안전하게 처리한 뒤 로그/부모 통지. stderr가 막혀도 전체 종료가 영원히 지연되지 않도록 로그 전송 상한·non-blocking/유실 표시 등은 부모 IPC/종료 설계에서 선택할 것. 조용한 초기값 reset 금지. 신뢰 불가 상태에서 서비스 재개 금지.
현재 로그 기능·전체 종료 전달은 아직 구현되지 않았으며 초기화 시험 성공과 혼동하지 않는다.

## 실행 결과를 구분
컴파일: make build/test_shared_backend_state 성공. C11/Wall/Wextra/Werror/ASan/UBSan/-pthread.
실제 실행: timeout20s 하네스 안에서 build/test_shared_backend_state exit0. 세 테스트 함수 모두 통과, sanitizer 진단 없음.
- layout: 0/범위 초과/예산 부족 거절, 예산 정확 경계·정렬 통과. 인덱스 범위 한계는 검사했고 32-bit size_t의 산술 overflow 실행 분기는 이64-bit 환경에서 별도 실행하지 못함.
- init: 0 기여·FREE 원장·IDLE record, live owner 덮어쓰기 거절, 생성/해제/빈 해제 통과.
- real fork: 자식의 공유 marker가 부모에 보이고, 자식 잠금 보유 중 SIGKILL 후 waitpid→EOWNERDEAD→테스트 marker 복원→consistent→unlock→재잠금 성공. 해당 시험은 실제 API 장애 시험이지만 원장·카운터 갱신 복구 시험 아님.
실패/skip: 이번 실행에서 없음. 미검증: attr/mmap/init 실패 주입, 거래 commit/rollback, 원장/통계/순번 쓰기 중 종료, 복구자 재종료, 상태 손상 진단, 전체 종료, 기존 서버 전체 회귀, 다른 OS. 제품 src/Makefile 기존 빌드 통합은 아직 아님. 성능/부하 미실행.
로그: build/iteration5-capability/initialization-test.log. 테스트의 alarm10s와 harness20s는 테스트 hang guard이며 운영 복구 제한이 아니다.

## 다음 합의 흐름
제한된 원장 변경과 undo commit 프로토콜 → 반복 가능한 rollback/owner-death 복구 → 세대별 회수 → 제한된 진단·부모 전체 종료 통지 순서로 구현/장애 주입/실물 함수 리뷰를 진행한다. worker별 로컬 event-loop wait/retry 방식·전체 상한의 실행 설정·통계/알고리즘 세부는 해당 흐름 구현 전에 선택한다. 재시작과 성능 승인으로 확대하지 않는다.
