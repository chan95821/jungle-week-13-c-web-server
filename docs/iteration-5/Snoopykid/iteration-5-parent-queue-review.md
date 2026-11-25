# Iteration 5 부모 FIFO와 수락 gate — 실제 코드 리뷰

2026-11-25. 사용자 채택: parent 전역 FIFO, W4/Q128/C256/G1152/R1024/1MiB 임시 상한, 포화 시 새 accept 보류·여유 시 재개, 기존 IPC/연결·종료 처리 계속. 일괄503/accept후즉시닫기 미채택.

## 이번 구현 범위
src/concurrency_limits.h에 채택한6개 값을 한 곳에 기록.
src/parent_client_queue.h/c에 parent-only 고정 ring FIFO와 gate 구현. 별도 mutex·공유 메모리·HTTP 파싱/정책 선택 없음.
현재 main/기존 서버 SRCS에 아직 연결하지 않았다. helper가 존재한다고 실제 서버가 이미 accept 보류/재개하거나4worker를 생성한다고 표현하지 않는다.

## main부터 연결할 순서와 실제 함수
후속 main: 설정/공유영역/queue/IPC/listener/fork 준비 → parent loop가 parent_accept_has_capacity를 보고 listener interest 제어 → accept/전체 장부 등록 → enqueue_parent_client → worker 선택 → dequeue_parent_client → prepare_worker_fd_transfer → STREAM 송신 → worker CLOSED 통보로 전체/worker 장부 해제 → 여유 시 listener 재개.
현재 코드:
1. create_parent_client_queue(capacity,&queue): 0/정수표현·곱셈 overflow/살아 있는 owner 덮어쓰기 검사 → 시작 시 배열1회calloc. capacity는 고정, 실행 중 확장 없음.
2. parent_accept_has_capacity: stopping=false, queue 정상·count<capacity, caller가 제공한 global_connections<global_limit인 경우만true. 포화/종료 때false. worker 상태·전역 장부를 내부에서 추측하거나 갱신하지 않는다.
3. enqueue_parent_client: 유효 metadata/공간 확인 → tail에 FD/ID/accepted_at 복사 → count증가, caller FD변수-1. full은 ENOSPC, caller 소유권 유지. actual parent loop는 accept 전 gate로 방지하고 예외 실패 cleanup은 후속 통합.
4. dequeue_parent_client: caller output FD=-1 확인 → head 이동·count감소 → 소유권 output으로 이동. 빈queue0, 이동1, 오류-1. FD는 닫지 않으며 worker 카운터를 임의 증가하지 않음(선택/장부에서 처리할 책임).
5. destroy_parent_client_queue: 남아 있는queued FD만 한 번씩close, close오류가 있어도 나머지 항목 정리 → free/owner0 → 최초 오류 반환. dequeue로 다른 소유자에게 넘긴 FD에는 영향 없음. Linux close EINTR 재시도 금지.
create/destroy는 heap 수명 경계, enqueue/dequeue/gate는 동적 할당 없는 O(1)이다. 신호 핸들러/다른 스레드가 queue를 직접 수정하면 안 된다.

## FD·메모리 예산
현재 WSL sizeof queued item32bytes, queue owner32bytes. Q128의 엔트리 배열4096bytes+owner32=4128bytes(allocator/페이지 비용 제외).
각 항목은 client FD1개·connectionID·원래accept시각만 보관한다. queue에 HTTP buffer/요청 본문·헤더·민감 입력 없음. listener/backlog·kernel socket buffers·worker active sender·전체 연결 장부는 이4128bytes에 포함되지 않는다.
parent가 보유할 원본 client FD의 설계 상한은 Q+W=132(그중 queue소유 최대128, 채널별 active sender 최대1개). 나머지 parent FD·실제 global/worker 장부의 예산은 해당 구현 후 재확인한다. 현재 count만으로 모든 FD 상한을 보장하지 않는다.
G1152에는 queue/worker에 배정되어 미종료인 연결을 한 번씩 포함해야 한다. queue를 비우거나 STREAM 전송으로 parent FD를 닫는 것만으로 global count가 감소하지 않는다. 현재 helper의 global_connections는 caller 입력이며 그 전체 장부는 미구현이다.

## 컴파일 확인
일반 -std=c11 -Wall -Wextra -Werror -O2 parent_client_queue.o 컴파일 성공.
독립 test_parent_client_queue ASan/UBSan 컴파일·링크 성공, 기존 STREAM product 소스를 함께 링크했다.

## 실제 실행 결과
새 unit 실행 exit0, 이후 make test 전체12개 바이너리 통과(failure/skip0, sanitizer 진단 없음).
- Q127/128/초과 enqueue, ID/time 보존,128회 채운 ring의 pop/push wrap FIFO 순서.
- invalid create/시간값·live owner 덮어쓰기·occupied output 거절, full일 때 caller FD 유지.
- destroy가 대기FD를close, 이미dequeue된FD는 유지, peer EOF·/proc FD 개수 복귀.
- close EBADF는 isolated fixture의 소유FD를 미리close하는 fault로 주입, 이후 다른FD도 회수/owner0/첫오류 반환 확인. production 외부owner doubleclose를 허용한다는 뜻 아님.
- G1151/1152/1153·stopping·0limit gate, 여유 후true.
- 실제 loopback listener+테스트 driver: Q1 full 동안 gatefalse라 accept를 호출하지 않고 다음 연결을 kernel pending에 남김 → dequeue 후 accept 재개 → queue가 비어도global2/limit2이면gatefalse. 실제 main/epoll 동작 시험은 아니다.
- FIFO→dequeue→기존 STREAM sender/receiver로 FD 전달, selected worker/ID/accept시각 보존, 연결 통신·종료·FD수 원복. worker는 fixture에서 명시 지정했으며 RR 등 정책 자동 선택 안 함.
로그 build/iteration5-capability/parent-queue-test.log(신규단위), unit-tests-with-parent-queue.log(12개).
테스트 loopback 외부 접속 없음, 실제 정적 root/권한/proxy.conf 미변경. 성능/부하 계측 없음. 임시 poll/alarm/harness 기한은 운영값이 아니다.

## 남은 통합/검증
- main의 listener epoll 등록·해제/재개, worker loop/생성/회수, 전체 연결 장부와 worker별 outstanding/C256 상한, 중복·늦은 CLOSED/사망 회수는 미구현.
- fatal 진단/control 채널·로그 적체에도 전체 종료, worker재시작/부모사망/종료기한은 미결정/미구현.
- queue 대기/partial frame 완료 기한·만료 처리, CLI/설정 입력, LT/ET·공정성/역압력,4정책 통계·실패 규칙은 후속 선택.
- calloc 실패 주입·다른ABI/OS는 미검증. 기존 서버make verify E2E 미실행, 제품 실행 경로는 미변경.
커밋/머지/푸시하지 않았다.
