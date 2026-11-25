# Iteration 5 — 검증 후보

2026-11-25: 모델 A 선택 직후 후보 등록. 아직 테스트 구현·실행 허락이나 통과 기록이 아니다.

- 기존 파서·라우팅·정적/동적/CGI·프록시 회귀, 바이너리·본문 경계·오류 우선순위 보존.
- 복수 클라이언트 동시 요청과 연결별 상태·본문 격리.
- 느린 클라이언트/백엔드와 빠른 정적 요청의 동시 처리. 작업자 간 분산만으로 가려지지 않도록 같은 작업자 내 격리도 검증.
- 백엔드 non-blocking connect 진행·SO_ERROR, 부분 recv/send, EAGAIN/EINTR, EOF/RST, 타임아웃.
- 응답 송신 시작 후 새 오류 응답을 붙이지 않기.
- 상한 도달·역압력·큐/버퍼 정책은 선택 후 통과 기준 추가.
- 작업자 비정상 종료·부모 종료·SIGINT/SIGTERM·CGI 회수 및 FD/메모리 누적 여부. 복구 정책은 선택 후 추가.
- 전체 공유 조회 상태를 기준으로 네 후보의 선택·동률·예약/해제·실패·작업자 종료 회수 검증 후보를 추가한다. 세부 동작·수단 확정 후 통과 기준을 정하며 기능 검증과 성능 비교는 구분한다.
- epoll 이벤트 후 해제된 연결 참조·FD 재사용·중복 close 방지.
- 부하·성능·1만 연결 실험은 별도 조건 합의 전 실행하지 않음.

- 정상·느린 백엔드·요청 비용 차이·장애·회복의 동일 조건 비교 후보는 iteration-5-distribution-comparison.md에 기록. 계측과 실행은 별도 허락 전 보류.

## 공유 상태 방향 추가 확정

공유 메모리 + 작업자 직접 선택을 채택했다. 부모 선택 조율 방식은 채택하지 않는다. API·잠금·복구·기여분·증감/통계·정책 입력 세부는 iteration-5-shared-state-design.md의 미결정 제안을 따른다. 경합·잠금 보유 중 종료·원장/카운터 회수·동일 비교 조건을 검증 후보로 추가했으며 실행은 보류한다.

## 1-A/2-A 확정과 복구 검토

익명 공유 mmap + process-shared robust mutex와 복구 후 계속 운영 목표를 확정. 다섯 장애 상황과 아직 선택하지 않은 자료구조·오류 대응은 iteration-5-recovery-review.md 참조. 단일 프로세스 API 초기화 지원 probe만 컴파일/실행 성공했으며 다중 프로세스·사망 복구·제품 회귀·성능 검증은 아니다.

## 복구 범위와 구조 A 확정 후 첫 구현 단위
사용자는 ①~④ 복구/재복구, ⑤ 탐지된 모순과 신뢰 불가 시 전체 안전 종료, 자료구조 A를 채택했다. 요청 원장/복구 기록에 상한을 두고 단계별 종료 주입을 검증하라는 지시는 기능 장애 검증 승인이다. 부하/성능 승인은 아니다.
첫 단위: 공유 매핑 계획/생성/해제. worker와 전체 reservation 슬롯 수 및 메모리 예산은 caller 입력이며 실행 기본값은 아직 정하지 않는다. 단일 공유 mutex로 한 번에 갱신하므로 undo record는 공유 영역에 1개만 배치, 여러 동시 journal은 만들지 않는다. 시간 통계·선택 알고리즘·복구 commit 프로토콜은 이 초기화 단위에 포함하지 않는다.
구현 전 테스트 기준:
- 0/인덱스 표현 범위 초과/size_t 산술 overflow/예산 부족은 매핑 전 실패, 출력 객체 미소유 상태 유지.
- 충분한 예산은 정확한 슬롯 수·정렬·초기 FREE 원장/0 기여분·유휴 복구 기록으로 생성, private/non-robust fallback 없음.
- 별도 fork 자식이 shared mutex를 trylock한 뒤 부모에게 알리고 SIGKILL됨. 부모 trylock은 EOWNERDEAD, 테스트의 변경 없는 상태에 consistent 후 재사용 가능.
- fork 자식의 공유 필드 변경이 부모에게 관측됨. 이는 복구 카운터 중간 변경 검증과 구별.
- 자식 종료/회수 후 mutex 파괴·munmap, 전체 매핑 크기는 예산 이내. 초기화 속성 실패의 errno 반환 및 부분 자원 정리는 코드와 주입 테스트로 확인(이번 단위에서 실패 주입 미구현이면 미검증 표기).
- 원장/기여분/전체 수/순번/통계 각 변경 단계 및 복구 중 SIGKILL은 후속 갱신/복구 구현과 함께 실행. 현재 단일 mutex owner death 시험을 그 전체 검증 통과로 표현하지 않는다.

## 구조 A와 복구 범위 채택 / 초기화 첫 구현 (2026-11-25)
사용자는 ①~④ 복구·재복구와 상한 있는 원장/복구 기록, ⑤ 제한 진단 로그 및 신뢰 불가 시 전체 안전 종료, 구조 A를 승인했다. 개별 수정 단계·복구 중 종료 주입 검증도 승인했다. 앞선 제안/선택 대기 문구는 이 결정으로 대체하되 통계·알고리즘·재시작·실행 상한 수치는 아직 미결정.
공유 상태 크기 계획/생성/해제와 독립 단위 테스트를 구현했다. 컴파일 성공 및 ASan/UBSan 단위 실행 성공: 예산 경계/초기화/실제 fork 자식 잠금 보유 중 SIGKILL 후 EOWNERDEAD 감지와 mutex 재사용. 실제 원장 갱신 복구가 통과한 것은 아니다. 상세 함수 리뷰·정확 메모리 예시·미검증 범위는 iteration-5-shared-state-initialization.md 참조. main 통합·전체 복구·로그/전체 종료·성능은 미구현/미실행.

## 원장 commit/rollback/회수 구현 전 기준 (2026-11-25)
- 정책은 아직 없음. trylock 성공~unlock 사이에 호출자가 선택한 backend와 다음 RR 순번(0/1)을 예약 API에 전달. 선택과 증가를 같은 잠금 구간에 묶되 RR 최종 채택이나 시간 통계 규칙을 도입하지 않는다.
- 단일 before-image 기록: IDLE→PREPARED→COMMITTED→IDLE. 대상 worker/reservation, 이전 전체 카운터·worker·예약·순번과 기록 checksum을 고정 크기에 저장. 시퀀스 일관 atomic phase와 fence로 이전 값 공개 후에만 대상 갱신. 미commit은 복원, commit은 보존. checksum은 임의 메모리 손상 방어/탐지 보장이 아님.
- trylock은 EBUSY를 즉시 반환. EOWNERDEAD는 원본 기록을 유지한 채 반복 가능한 복원→전체 불변 조건 검사→IDLE→consistent. 복구 중 임의 지점 사망 후 다시 복구해야 함.
- worker 세대 시작/RETIRING/예약별 해제/RETIRED는 개별 트랜잭션. 부모가 waitpid로 사망 확인한 뒤만 회수 API 호출. 대체 작업자 자동 생성 없음.
- 예약마다 generation token, 완료 중복·과거 worker/request token은 무변경. wrap/포화는 errno 오류, 세대/숫자 reset 없음. runtime HTTP 오류 매핑은 아직 결정하지 않는다.
- 프로세스별 검증 scratch를 시작 시 할당해 recovery 검사 O(R+WB), 잠금 안 malloc/I/O 없음. 정상 경로는 대상 항목 검사와 고정 크기 기록만. fatal이면 bounded local diagnostic을 반환, 로그 I/O/부모 전체 종료 전달은 후속 통합.
- 정상 예약/완료/상한/세대 재사용/중복 완료/부모 세대 회수 검증.
- journal 공개 전/후·예약·기여·전체 수·순번 쓰기·commit·IDLE 각 단계 SIGKILL. commit 경계 전후 예상 상태, survivor의 예약/기여·순번 보존, 회수 반복 무변경.
- rollback의 예약/worker/전체/순번 복원·검증/consistent 각 단계에서 복구자 SIGKILL 후 재복구 검증.
- 회수 RETIRING/각 예약 해제/RETIRED 변경마다 사망, 재회수 후 죽은 세대0/다른 세대 불변.
- 잘못된 상태/인덱스/기여 합계/원장/checksum을 임시 매핑에 주입, 신뢰 불가 진단과 fatal 표시, 새 갱신 금지. 정상처럼 일치하는 임의 손상 탐지 보장 제외.
- 4개 fork 작업자의 제한된 예약/완료 반복은 경합 정합성 기능 검증이며 처리량/시간 지표 수집 없이 실행. 부하/성능 비교 아님.

## 원장 commit/rollback·재복구 단위 완료 (2026-11-25)
src/backend_reservations.c의 예약/완료·세대·회수·trylock/owner-death 복구와 bounded diagnostic을 구현했다. 고정 undo1개128bytes, header232/worker40/reservation32, scratch24W bytes/프로세스. 메모리 예시·actual 함수 순서·불변 조건·한계는 iteration-5-reservations-review.md 참조.
make test 10개 통과, 추가 commit 보존 경계를 포함한 최종 새 테스트도 통과: 변경8+8·generation6·재복구7+6·회수20·손상6+3·4프로세스 강제경합. 테스트는 실제 fork/SIGKILL이지만 원장 단위이며 TCP 서버 worker E2E가 아니다. main/epoll·부모 전체 종료는 미통합. 정책/시간 통계·실행 상한·재시작·부하/성능 미결정·미실행.

## 부모·작업자 IPC 다음 설계 단위

iteration-5-ipc-design.md에 main부터 FD 인계·fatal 통지 흐름과 전송/종료/FD 회수 검증 후보를 기록. socketpair SEQPACKET/STREAM, worker 배분, 인계 기준·큐·포화·fatal 우선순위/종료는 아직 선택 대기. 제품 코드·서버 실행·성능 변경 없음.

## IPC STREAM 채택

작업자별 SOCK_STREAM 채널을 선택했다. 부분 sendmsg의 FD 중복 전송 방지·프레임 경계/ancillary 귀속·미완료 프레임 EOF 정리 검증을 후보로 반영. worker 배분·상한·인계·종료 정책은 미정. iteration-5-ipc-design.md의 최신 결정 우선.

## worker/backend 공통 선택 계산 검증 후보

worker도 네 정책 지원 확정. 동일 입력의 공통 계산 일관성·대상 수 차이·독립 순번/상태·worker 종료/이전 세대 통보·정책 설정 혼동 방지를 검증한다. worker 진행 수·시간 규칙 합의 후 통과 기준 추가. 성능 비교는 다른 층 정책을 고정해 제안하고 별도 승인 전 실행하지 않는다.

## worker 진행 수 기준 확정

worker 진행 수는 배정 후 미종료 연결 수로 확정. slow header/body·backend 대기·송신 연결 집계, 종료 중복/늦은 통보·generation 재사용·부분 FD 전송과 원본 사본 수명 검증 후보를 추가한다. 증가/인계/ACK/포화의 세부 기준은 후속 선택 후 통과 기준을 확정한다.

## STREAM FD 인계 첫 단위 — 구현 전 검증 기준
사용자1번 전체 프레임 전송 후 close/no-ACK 채택. 단위 검증 범위는 실제 socketpair/fork 전달·부분 I/O·오류/EOF·FD 회수이며 성능 지표는 수집하지 않는다.
- socketpair가 STREAM/nonblocking/CLOEXEC. parent 소유 원본은 전체 프레임 전까지 유지, 완료 후 close, pointer 인계로 중복 소유 방지.
- test-only syscall wrapper가 실제 sendmsg 길이를 제한하여 양수 short send를 강제. FD ancillary는 첫 양수 송신1회만, EINTR/EAGAIN은 상태 보존·즉시 반환.
- receiver는 첫 바이트로 FD를 받고 완전한48bytes 이전에는 client를 공개하지 않음. 두 frame 동반 도착에도 서로의 FD/metadata를 섞지 않음.
- 버전/type/magic/예약필드·timestamp·worker세대·ID 검증, FD누락/복수/잘린 control/본문 단계 예상외 ancillary는 받은 모든 FD 정리.
- 부분 EOF는 보관한 FD close, channel error·sender peer 종료는 원본 회수, 취소 반복 무변경. fd 번호 재사용을 고려해 단순 숫자 비교만으로 leak 판정하지 않음.
- 실제 fork 후 inherited client 사본을 닫고 받은 FD로 통신. receiver가 recv 전/부분수신 후 종료하면 queued 참조·sender 원본이 정상 정리되어 peer EOF.
- /proc/self/fd로 정상·오류·반복회수 후 FD 개수 복귀 확인(테스트 Linux/WSL 한정). 테스트 timeout은 운영 timeout/성능 측정 아님.

STREAM 보강 후보: 실제 테스트 socket의 송신 buffer를 채워 EAGAIN을 확인한 뒤 drain하고 동일 FD 전송 재개, non-connected stream FD의 거절, MSG_CTRUNC 실제 관측과 전달된 FD 회수. 커널 buffer 조건은 테스트 소켓에만 적용하며 부하/성능 계측하지 않음.

STREAM 수신자 종료 보강: 자식이 inherited client FD를 닫은 것을 pipe로 확인한 뒤 recv 전/anchor 수신 후 두 지점에서 부모가 SIGKILL. 커널 queued/수신 보유 참조와 sender 잔여 원본 정리 후 peer EOF, FD 개수 원복을 통과 기준으로 한다.

## STREAM FD 전송 단위 구현·검증 완료 (2026-11-25)
src/worker_fd_channel.h/c 구현, 전체 frame 송신 후 원본 close/noACK, partial send의 FD1회·frame 검증 전 private 보유·오류/EOF FD 회수. 실제 fork/수신 전·부분 수신 후 SIGKILL·MSG_CTRUNC·실제 EAGAIN·FD0/FDcount 검증 통과. 일반 컴파일 성공, make test ASan/UBSan 11개 통과. 실물 함수 순서/소유권/한계는 iteration-5-ipc-transfer-review.md 참조.
main·운영 worker/queue/epoll·공통 네정책·fatal 제어 통지/전체 종료는 미통합. 서버 동시성 완료/부하·성능 통과로 표현하지 않는다. 운영 상한·미결정 정책 자동 채택 없음.

## 부모 queue/포화 다음 선택안

iteration-5-parent-queue-design.md에 queue 위치·Q/W/C/G 상한·FD/메모리 산식·listener 보류/재개·경합/회수 후보를 기록. 모두 선택 대기, 코드·검증·성능 실행 없음.

## 초기 상한 승인 후 검증 후보

W4/Q128/C256/G1152/R1024/budget1MiB 임시값 채택. Q127/128/129, C255/256/257, G1151/1152/1153의 거절/보류·counter/FD 정확성 후보를 추가한다. 실제 경계 동작은 포화 정책 선택 후 확정. 작은 테스트 capacity로도 동일 불변 조건을 검증하며 성능 부하로 확대하지 않는다.

## 부모 FIFO/수락 gate — 구현 전 기준
- create0/산술범위 초과 거절, 살아 있는 queue 덮어쓰기 거절, 고정capacity128 ring FIFO wrap 순서와 ID/accept시각 보존.
- enqueue는 FD 소유권을 이동해 원본 변수를-1, full ENOSPC는 caller 소유권 그대로, dequeue는 output에 이동. 비어 있으면0, output이 이미 FD를 소유하면 덮어쓰기 금지.
- destroy는 queue가 소유한 FD만 정리, dequeue로 옮겨진 FD에는 영향 없음. 반복 destroy/FD개수 원복/peer EOF로 검증.
- Q127/128/129, G1151/1152/1153, stopping/여유 재발생 시 gate false/true. G는 아직 caller가 제공하는 전체 장부 집계이며 이 단위가 그 장부를 구현하지 않음.
- 실제 loopback listener에서 Qfull일 때 gate를 따른 테스트 흐름은 accept를 호출하지 않고 kernel pending을 남김. dequeue 후 재개, queue가 비어도 Gfull이면 보류. epoll 모드/감시 등록 제어는 미통합.
- dequeue→기존 prepare/send/receive STREAM API 연결, metadata/FD 소유권 보존과 완료close 확인. 선택 정책은 테스트의 지정worker만 사용, RR 자동 채택 아님.
- 작은fixture/단위 경계만 실행, 부하·성능 계측 아님. 실제 staticroot/운영권한/config 미변경.

## parent FIFO/gate 단위 완료 (2026-11-25)
concurrency_limits.h의 임시 상한6개, parent_client_queue.h/c의 고정ring FIFO·FD이동/정리·queue/global/stopping gate 구현. 일반 컴파일/ASan·UBSan unit 성공, make test12개 통과. Q128wrap/full·G경계·FD count/cleanup 및 실제loopback 테스트driver의 accept보류/재개·STREAM연결 검사 통과. 상세 iteration-5-parent-queue-review.md.
main/실제epoll·전체연결장부/workerC256/종료통보·fatal전체종료는 미통합. FIFO4128bytes와gate는 전체서버자원/동시성보장이 아니다. 성능/서버E2E 미실행.

## epoll LT 채택 (2026-11-25)

사용자1번으로 Level-Triggered 확정, ET/ONESHOT 미채택. 별도긴급pipe/진단슬롯 vs기존STREAM재사용 제어통지는 선택 대기. 비용/범위/검증 후보는 iteration-5-control-notification-design.md 참조. 이번에는 문서만 변경, 코드·검증·성능 실행 없음.

## 긴급 pipe/slot 첫 단위 — 구현 전 검증 기준
- 익명shared mapping의worker별고정slot과lock-free generation/publication init; 필요한bytes/budget검사유지. 진단slot에process-local pointer/FD없음.
- publisher는backendmutex밖에서동작, 자기slot/currentgeneration만게시, 중복게시firstreport유지. parent는READY acquire후에만payload복사, EMPTY/WRITING/STALE에서는output불변. 재arm은waitpid후generation증가만허용, shared fatal flag를조용히reset하지않음.
- publishintent/fatal표시→payload→READY→pipe write 경계SIGKILL. 미완료는unknown/nopeek, READY는writer사망후에도조회가능. pipe write이전사망에는pipe알림이없을수있다는한계도검증.
- pipe nonblocking/CLOEXEC, 평시빈상태·1byte알림·LT epoll wake·EOF, 실제pipe포화/EAGAIN에서추가block없음, EINTR주입은retry결과반환, parent read end없을때EPIPE와진단보존. SIGPIPEignore는기존서버계약(테스트만ignore설정), 함수안에서전역시그널정책변경하지않음.
- 일반STREAM송신buffer가full이어도별도pipe의epoll 알림/READY진단확인. 다중fork worker 각slot동시게시, oldgeneration늦은게시/진단손상은정해진오류반환, 범위밖payloaddump금지.
- 모든test socket/pipe/fork·mapping정리와FD수복귀, ASan/UBSan. 컴파일/실제실행/서버전체종료E2E미통합/성능미실행구분.

긴급게시 경계 보강: fatal 의도를 slot claim 전에 표시하고 그 직후에도 SIGKILL. 이 경우 slot EMPTY라도 shared fatal은1이어야 하며 부모가 EMPTY를 안전 상태로 오해하면 안 된다. 준비안된 payload는 읽지 않고 원인unknown으로 남긴다. 기존global fatal후에는 다음세대arm을 거절하여 증거를 보존한다.

## 긴급pipe·고정진단 단위 완료 (2026-11-25)
worker_failure_channel.h/c와version3 sharedslot 추가. fatal의도→WRITING→READY 게시, firstreport유지·generation차단·READY전payload미조회, 별도nonblocking pipe알림/소비와제한숫자/unknownformat 구현. slot64bytes·W4추가256, 초기mapping33416bytes/budget1MiB유지.
일반컴파일성공, make testASan/UBSan13개통과. 실제LT/pipe·STREAM포화/EPIPE·5경계SIGKILL/다중게시/손상/복구불가mutex·FD회수검증. 상세 iteration-5-failure-channel-review.md. main/SIGCHLD/실제전체종료·로그sink적체/기한은미통합이며특히알림전사망은SIGCHLD경로가필요하다. 서버E2E/성능미실행.

## 시그널·epoll 경합 다음 선택안

epoll_pwait+최소 handler / signalfd의 비용·main 흐름·fork/CGI 회수 경계를 iteration-5-signal-design.md에 기록했다. 현재 선택 대기이며 코드·검증·성능을 실행하지 않았다.

## epoll_pwait 단위 — 구현 전 검증 기준
- 설치중SIGINT/TERM/CHLD차단, 대기mask에서만허용, SIGPIPEignore. handler가추가I/O/할당/정리를하지않고caller의제어상태만변경.
- 차단상태에서pending시그널및검사후wait진입직전실제kill/raise를주입, handler가그전실행되지않고실제epoll_pwait에서EINTR/상태반영. 종료0은이후CHLD에도0, childbit소비도종료를되살리지않음.
- 비동기child가보낸INT/TERM과실제child종료, 다중종료의SIGCHLD병합→호출자waitpid(WNOHANG)가자기PID들을모두회수. helper가먼저자식을reap하지않음.
- 준비된I/O와pending종료가동시에있어도종료를계속미루지않는loop규칙검증, mask는wait후다시차단상태.
- 긴급READY/WRITING게시후pipe쓰기전worker사망을SIGCHLD로확인해sharedfatal과ready/unknown을찾는통합fixture. main/실제TCPpoolE2E와구분.
- 원래mask/disposition복원, fork/exec자식의관리signal과SIGPIPEdefault·원래mask복원, parent상태불변. 단순execprobe와실제CGI완료조건은구분.
- 새context한개당heap/추가FD없음. 실제SIGPIPEignore등은test프로세스에서복원하고fixture자식/FD/mapping회수. 컴파일/실제signal시험/부하성능미실행구분.

## 시그널 첫 실행 실패·보강 선택 대기 (2026-11-25)
process_signal_wait.h/c와새단위를구현/컴파일. pending/대기직전6개통과, 항상readable LT I/O에서pending종료우선반례실패. 이후시그널시험미실행, 새단위포함전체make test는통과상태가아니다. main미통합/성능미실행.
원인·원문·두수정안과예상syscall비용은 iteration-5-signal-review.md 참조. wait반환후잠깐unmask/reblock(추천) vs pending조회/0초소비는사용자선택전구현하지않는다. 기존epoll_pwait선택·최소handler·HTTP정책을임의변경하지않음.

## wait 반환 구간 보강 확정 (2026-11-25)
사용자 ‘1반’을 1번 선택으로 확인했다. epoll_pwait 반환 직후 wait_mask를 잠깐 적용해 pending 관리 시그널을 handler에 전달하고 managed_mask를 다시 차단한다. caller는 I/O보다 종료·fatal·자식 상태를 먼저 검사한다. 두 mask 호출이 성공하면 원래 wait 반환값과 errno를 보존한다. mask 변경 실패는 -1로 보고하고, 첫 호출 실패 시에도 재차단을 시도한다. 호출마다 mask 변경 두 번이 추가되며 성능은 측정하지 않는다. 기존 ready I/O 실패 반례는 유지한다.
검증 범위: 기존 시그널 시험 전체, ready 반환값·차단 마스크, EINTR/EBADF 보존, timeout 반환, 기존 단위 회귀. main 통합·실제 CGI·부하 측정은 포함하지 않는다.

## 시그널 보강 검증 완료 (2026-11-25)
1번 구현 후 기존 ready I/O 실패 반례와 시그널·fork/exec·FD 회수 시험 통과. 일반 O2 컴파일 성공, ASan/UBSan 전체 make test 14개 성공(exit 0). 반환값·errno·차단 mask 보존도 확인했다. sigprocmask 실패 강제 주입, main/실제 worker/CGI 통합 E2E는 미실행. 성능 미측정. 상세 흐름·비용·한계는 iteration-5-signal-review.md에 기록했다.

## 부모 장부·CLOSED 확정 검증 후보 (2026-11-25, 구현 전)
- 등록/배정/완료/취소/회수마다 total=queued+assigned, worker별 수=해당 세대 assigned 검증.
- G/C 경계, 재사용 세대·wrap, 중복·과거/미래·다른 소유권 통지, 다른 worker 불변.
- SENDING에서 CLOSED를 먼저 받은 뒤 handoff 완료, 사망 회수 전/중/후 중복 통지.
- 실제 STREAM partial send/recv, EAGAIN/EINTR, 동반 프레임, malformed/EOF/예기치 않은 FD 정리.
- worker CLOSED 큐 full에서 새 FD 수신 gate false, 통지 전송 후 재개; 정상 채널 역방향 FD 인계와 공존.
- 테스트 자식의 전송 전/부분 전송/전송 후 SIGKILL 후 waitpid를 확인한 회수 및 FD 개수 복원.
- FIFO와 기존 FD 인계·accept gate를 조합한 기능 fixture. 실제 서버 main·종료 범위·성능은 제외.

## 연결 장부·CLOSED 단위 완료 (2026-11-25)
사용자 1번을 parent_connection_ledger.h/c 및 worker_closed_channel.h/c로 구현했다. QUEUED/SENDING/OWNED와 early CLOSED, 토큰 재사용 세대, 정확1회 해제·waitpid 이후 회수, bounded worker 큐와32byte STREAM 부분 I/O를 제공한다. 장부/완료 큐는 FD를 소유하지 않는다. 실제 W4/C256/G1152 경계·부분 I/O/실제EAGAIN·비정상 ancillary·3경계 SIGKILL 회수 포함 ASan/UBSan make test 15개 통과. 일반O2 컴파일 성공. 장부+완료큐+수신 상태62,104bytes(현재ABI), 다른서버자원 제외. main/실제epoll/서버E2E·성능 미통합/미실행. 상세 iteration-5-connection-completion-review.md. 이전 ‘제안/미선택’ 기록은 이 선택과 결과로 대체한다.

## 감독 흐름 구현 전 검증 범위 (2026-11-25)
- 실제 fork 작업자 2개: 하나의 채널 오류 후 SIGTERM 종료·회수, 다른 작업자 socket I/O/예약/연결 기여분 유지.
- SIGTERM 무시 자식: 종료기한 직전 SIGKILL 없음, 기한 후 실제 SIGKILL·waitpid, 반복 호출 중복 신호/회수 없음.
- 전체 SIGINT/SIGTERM 표시·shared fatal·마지막 worker 사망에서 accept 불가, FIFO 원본 FD/queued 장부 정리, 종료 중 재배정/자동 fork 없음.
- 사망 worker의 partial sender 원본·채널 정리 후 연결·backend 원장 회수, 다른worker 세대 유지.
- 다른 worker가 공유mutex 보유: trylock EBUSY 뒤 pending 반환, 다른 worker 데이터·종료 처리 진전, unlock 알림 뒤 재회수.
- 실제 epoll_pwait + SIGCHLD 연계, 신호 병합/대기중 종료와 이미 ready인 I/O, 자식PID별 WNOHANG 회수.
- 공유원장 모순/복구불가의 제한된 숫자 진단·전체종료, 모두 waitpid 확인 전 destructor EBUSY.
- 테스트 훅으로 waitpid EINTR/kill ESRCH/오류 분기 확인. 성능/부하 및 제품main E2E 제외.

## 부모 감독 단위 완료 (2026-11-25)
parent_worker_supervisor.h/c에 PID별 WNOHANG·격리·임시2000ms 유예/TERM/KILL 각1회·waitpid 확인 후 sender/채널→연결장부→backend 원장 회수를 구현했다. shared EBUSY는pending, 신뢰불가면전체종료, 유효worker0 오류종료, 자동대체없음. 첫숫자진단고정보존·ECHILD시PID신호/파괴차단. 일반모듈O2와make all성공, 실제fork/신호/잠금경합 포함ASan/UBSan make test16개통과. W4추가560bytes(현재ABI), 성능미측정. 상세 iteration-5-supervisor-review.md. main/실제HTTP worker·log sink·부모사망·정책점수·EBUSY 재시도 wake/간격은미통합/미결정이며 테스트의20ms를운영정책으로채택하지않음. make verify/부하/커밋/머지/푸시없음.

## 10ms 재시도 구현 전 확정 검증 후보 (2026-11-25)
- 실제 fork의 mutex EBUSY 후 기한 전 반복 advance/ready I/O에도 trylock 호출 수 증가 없음, 기한 후 한 번만 시도.
- 마지막 외부 I/O로 unlock한 뒤 추가 요청/알림 없이 빈 epoll의 timeout으로 회수 완료.
- 두 사망worker의 서로 다른 재시도기한, 각각 due인 worker만 재시도, 다른worker I/O 진전.
- 실제 종료한 자식의 SIGCHLD를 먼저 소비하고 waitpid EINTR를 주입한 뒤 새시그널 없이 타이머 재확인·회수.
- 종료기한과 재시도기한의 최소값, 0/1ms/기한전·정확히·이후/해제, 역행·오버플로우 시간 거부.
- pending 중 shared fatal/종료 우선 처리, 종료 SIGKILL 기한을 재시도가 연장하지 않음.
- 기존 16개 ASan/UBSan 회귀, 일반 컴파일 구분. 테스트의 호출수/시간검사는 기능기한 확인이며 성능측정이 아니다.

## 10ms 제어 재시도 완료 (2026-11-25)
사용자1번을 worker별 WAIT/BACKEND retry_kind/retry_at_ms와INITIAL_SUPERVISOR_RETRY_MS=10으로 구현했다. 기한전같은작업보류·due시1회·pending해제·sharedfatal우선, parent_supervisor_wait_timeout_ms로종료/재시도최소기한 제공. 추가요청없는실제epoll타이머회수·ready I/O trylock제한·worker별기한·새CHLD없는EINTR재회수 통과. 일반O2/make all성공, ASan/UBSan전체16개통과. W4감독저장592bytes(이전보다32증가). 상세 iteration-5-supervisor-retry-review.md. main/HTTP worker미통합, E2E/성능미실행. 이전재시도방식미결정문구는이번선택으로대체.

## backend lifetime 구현 전 단위 검증 (2026-11-25)
동일잠금아래선택결과예약·시도전진행수증가, ACTIVE→RELEASE_PENDING→RELEASED;정상/실패/취소각1회해제;mark중복첫결과유지;pendingreset/begin거부;EBUSY토큰보존/다른worker기여유지;실제forkSIGKILL ACTIVE/pending/released세경계의부모세대회수;이전토큰이재사용예약해제못함;공유fatal해제오류토큰유지. client출력이남은fixture의backend해제와worker연결count유지. 기존proxy protocol단위는프레이밍회귀이며새HTTP상태머신의EOF/chunk/half-close/RST연결검증은미통합후보로구분. 성능미실행.

## backend lifetime 단위 완료 (2026-11-25)
backend_request_lifetime.h/c에begin_locked→ACTIVE→mark_finished→RELEASE_PENDING→try_release→RELEASED/reset구현. 공유선택과예약동일잠금·EBUSY토큰보존·중복첫outcome유지·재사용보호. 실제fork경합/3경계SIGKILL·parent회수·잔여client출력/worker수분리·half-close socketpair시험포함ASan/UBSan전체17개통과, 일반O2/make all성공. 객체32bytes(현재ABI). 실제HTTP프레이밍완료hook/worker상태머신·release재시도미통합/미결정;기존parser회귀와구분. 성능/E2E/커밋/머지/푸시없음. 상세 iteration-5-backend-lifetime-review.md.

## 정책 묶음 코드 전 검증 (2026-11-25)
네정책/정확동률cursor/미측정우선/32번째탐색, EWMA첫값/상하차분/최소1us/포화곱/오버플로우·역행시간거부, 실패최소1초/HTTP완료·취소분리. backend선택+예약+순번/탐색counter동일transaction, 완료표본+감소동일transaction, 중복·과거token표본불변. 모든기존SIGKILL/재복구경계와새통계갱신/복구경계. CLOSEDv2부분I/O/결과/시간변조·early중복보존·stale세대차단. 필수config누락/중복/미지원값/기존fixture명시값. 컴파일/단위/제품E2E분리, 성능비교미실행.

## 네 분배 정책 구현·단위 검증 완료 (2026-11-25)
공통 distribution_policy(네정책/EWMA1/8/32회탐색/실패floor1초/포화곱/정확동률), sharedv4 통계+선택예약/완료해제 transaction과복구checksum, backend고정시간구간, parent관측CLOSEDv2(56byte)/선택배정, 필수worker-policy/query-policy설정 구현. 순차query경로도정책/표본연결;worker-policy의실제multiworker main연결은아직미통합. 예제config/proxy.conf.example 추가, 개인build설정불변. 일반O2/make all·ASan/UBSan전체18개통과. 통계변경10·선택변경8경계SIGKILL/재복구/2프로세스경합 검증. sharedmapping33528bytes(W4/R1024),상세비용/함수흐름 iteration-5-policy-review.md. 제품make verify/E2E·부하/성능·커밋/머지/푸시없음. 최종알고리즘미정유지.

## worker I/O 코드 전 검증 범위 (2026-11-25)
실제fork worker+epoll+기존SCM_RIGHTS/CLOSED, loopback C backend·Python은미사용단위드라이버. 지연header/backend/느린client중다른echo·제출진전, binaryPOST/분할/100/조기거절·8/64KiB, backend고정/청크/EOF/1xx/무본문/오류·최종후두번째오류없음,relay상한/보류재개·idle예산/전체기한,선택lock503/해제pending·10ms기한,staleeventtoken·FD/메모리정리. pure계산의주입시각과실제TCP실행분리. localfile/CGI제품경로미통합,기존단위회귀18개. 부하/성능미실행.

## 실제 worker nonblocking 네트워크 단위 완료 (2026-11-25)
worker_http_io.h/c·worker_http_internal.h·worker_proxy_io.c에기존FD/CLOSED채널+실제LT epoll다중연결·요청/100·backend선택/connect/송신/헤더/본문·16/8KiB역압력·worker10ms/선택1초503·해제pending·전체/idle기한·예산·세대eventtoken구현. 기존pureheader/parser재사용. FD전달중증거있는TCPreset은CLIENT_GONE으로연결취소처리,다음frame/FD회수검증. strictmake all/worker-io및ASan/UBSan전체20개통과,실제fork/TCP/Cbackend/4MiB binary/느린A중B·제출진전/네정책/SIGTERM대기중회수확인. 상세 iteration-5-worker-io-review.md. 제품main은여전히순차,localfile/CGI/부모실행통합/긴급통지최종조율미완료. make verify제품E2E·부하/성능·커밋/머지/푸시없음.

## 부모 실행 루프 연결 — 최소 검증 후보 (2026-11-25)
실제parent listener1개→공유정책으로4worker분배→FD/CLOSED→로컬메모리/프록시요청. 느린query A 대기중query B/제출/hello완료,정상SIGTERM시4worker회수·FD/장부/예약정리. compile/link와이짧은실행만이번에수행. 기존20개전체회귀·광범위SIGKILL·부하/성능은실행하지않는다.

## 부모·4작업자 HTTP 통합 최소 확인 (2026-11-25)
parent_http_loop의 단일 리스너→FIFO→worker 정책→SCM_RIGHTS→CLOSED→감독/회수 흐름과 기존 hello/echo/greet 메모리 응답을 실제 reactor에 연결했다. strict O2 컴파일·링크와 짧은 실제 fork/TCP 통합 실행 통과: 느린 A 중 B/제출/로컬 응답 완료, 같은 작업자의 greet 진전, SIGTERM 후 4worker/FD/장부/예약 회수. 최초 테스트 fcntl.h 누락 컴파일 실패는 수정했다. 이번 변경 후 전체 회귀·광범위 장애·성능은 실행하지 않았다. 제품 main은 순차 유지, 정적 파일/CGI I/O는 별도 선택 후 연결한다. 상세 iteration-5-parent-integration-review.md. 실행 make smoke-process-http는 기능 최소 검사이며 성능 측정이 아니다.

## 파일 보조 스레드 1번 채택 및 구현 전 최소 확인 (2026-11-25)
작업자별 제한된 파일 보조 스레드를 채택했다. 첫 구현은 작업자당 보조 스레드 하나(성능 결론이나 최종 튜닝값 아님), 기존 C 연결 상한에서 유도한 작업 큐, 연결별 최대 한 작업이다. 파일 준비/read/provider cleanup만 맡기며 네트워크·공유 backend 잠금은 건드리지 않는다. 기존 16KiB provider 버퍼를 빌리고 현재 조각 송신 후 다음 읽기를 요청한다. 취소 중 작업은 슬롯과 provider를 유지하고 보조 스레드 완료/정리 후 CLOSED를 보낸다. 작업중 디스크 syscall을 강제 취소하지 않는다. 종료는 join을 기다릴 수 있고 기존 부모 2초 유예/SIGKILL/waitpid가 최종 경계다. 파일 전용 완료 pipe를 epoll에 추가한다. CGI fork는 호출하지 않고 별도 비동기/다중스레드 안전 통합으로 남긴다.
최소 확인: 보조 스레드가 테스트용 느린 파일 작업을 수행하는 동안 호출자 진행, 제한 큐/한 작업/정리; 실제 4worker 통합에서 임시 fixture 정적 파일의 다중 조각 바이너리 일치·기존 요청 동시 처리·종료 FD 회수. 전체 회귀·광범위 장애·성능은 이번 실행에서 제외한다.

## 파일 보조 스레드 통합 완료 (2026-11-25)
worker_file_io/worker_file_bridge로 worker당1helper·C상한큐·job당한작업·기존16KiB provider 스트리밍·완료pipe/epoll·취소후정리전slot/CLOSED보류를 구현했다. 일반O2 all/worker-io/두 최소실행 링크성공. 느린파일testhelper중호출자진행·실제4worker 임시fixture128KiB binary/빈파일/404/cafe·느린query중다른요청진전·SIGTERM후FD/예약회수통과. signal mask는pthread_sigmask(일반)/sigprocmask(exec자식)로분리하고 직접관련ASan/UBSan signal대상통과. 최초featuremacro컴파일실패는수정. 전체회귀/광범위장애/TSan/성능미실행. helperjoin은diskcall을기다릴수있고2초KILL도커널D상태회수를보장하지않음. 제품main은기존순차,CGI안전한비동기실행/회수후전환예정. 상세 iteration-5-file-io-review.md.

## CGI 비동기·제품 진입점 연결 전 범위 (2026-11-25)
기존 CGI 경로·Status/Content-Type·chunked·헤더/읽기5초·종료오류 정책을 유지한다. 경로/실행env준비는파일helper,네트워크스레드fork자식은async-signal-safe FD/시그널/exec만사용. 비차단stdout파이프·한조각역압력·PID별WNOHANG단일회수자·취소TERM/기존200ms후KILL을eventloop에연결한다. exec/CGI실패는기존502/500,최종송신후새오류미첨부. helper완료·CGI자식회수전슬롯/CLOSED보류. 기존CLI설정필수·4worker/1listener·공유분배를제품main에연결한다.
최소확인후보: 기존CGI정상/query/바이너리chunk·느린CGI중동일worker빠른로컬응답·잘못된CGI/실행실패·클라이언트취소·종료자식/FD정리,제품실행진입점정상기동/종료. 임시fixture만변경한다. 전체회귀·광범위장애는통합체크포인트후검토,성능미실행.

## CGI·제품 main 통합 체크포인트 (2026-11-25)
기존 CGI 정책을공통함수로추출,helper launch준비→안전한fork/exec자식→nonblocking stdout/청크/역압력→PID별WNOHANG→취소TERM/기존200msKILL·회수로연결. 제품main이실제부모/4worker/epoll/공유분배를실행한다. strictO2제품링크와smoke-file-io/smoke-process-http/smoke-process-server통과. 최초Makefile변수순서링크실패수정. 실제제품CGI정상/잘못된출력/exec실패/비정상종료·느린CGI중모든worker진전·RST취소FD기준선·살아있는CGI중SIGTERM회수통과. 전체회귀/광범위장애/성능미실행. stdoutEOF후계속생존CGI의별도종료기한/worker강제사망시후손감독은미결정후속후보. 이전main순차·CGI미통합기록은이결과로대체. 상세 iteration-5-cgi-main-review.md, 로그cgi-product-build.log/cgi-product-minimal.log.
