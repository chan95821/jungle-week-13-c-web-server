# Iteration 5 — 고정 프로세스 풀과 프로세스별 epoll

2026-11-25 사용자 선택. 기준: Iteration5/concurrency_model-YYJ / 577a554.

## 확정 범위
- A: 시작 시 고정 수 작업자 프로세스를 만들고 각 프로세스의 non-blocking epoll 이벤트 루프로 여러 클라이언트·백엔드 연결을 관리한다. 연결마다 작업자를 만들지 않는다.
- 외부 접속 주소·포트 하나, 기존 정적·동적·CGI·Reverse Proxy 기능과 HTTP 계약 유지.
- 연결별 상태·버퍼·FD 소유권과 부분 I/O 진행 위치·기한을 명시한다.
- B·C 구현, 인증·캐싱, 자동 최적화, 성능 측정은 이번 선택에 포함하지 않는다.

## 비교 제안과 채택 구분
| 모델 | 구성 | 비교 목적 |
| --- | --- | --- |
| A (개인 채택) | 고정 프로세스 풀 + 프로세스별 epoll | 장애 격리·다중 연결 |
| B (비교 후보) | 고정 스레드 + 스레드별 epoll | 공유 주소 공간·동기화 비용 |
| C (대조군) | accept 담당 + 제한 큐 + blocking 스레드 풀 | 코드 재사용·이벤트 루프 효과 |

팀 전체 채택이나 세 구현 동시 개발 지시로 해석하지 않는다. A/B의 이벤트 처리 조건은 가능하면 맞춘다. 동일 요청·백엔드·HTTP·빌드·타임아웃·분배 조건을 비교하며, 총 연결 수와 실제 진행 요청 수를 구분한다. 작업자 1·2·4·8 및 초기 4는 검토 후보이며 아직 설정 확정은 아니다. C의 16·32 스레드는 별도 조건이다.

## 구현 전 남은 선택
- 연결 수락 확정: 부모가 accept 후 IPC로 작업자에게 FD 전달. IPC·분배·상한·소유권 인계 세부는 미결정.
- 조회 상태는 서버 전체 공유 방향으로 확정. RR / Least Outstanding+동률 RR / 응답 시간+동률 RR / 진행 수·시간 결합 점수+동률 RR 네 후보 등록. 공유 메모리에서 작업자가 직접 선택하는 방향은 확정. 최종 알고리즘·공유 메모리 API·동기화·비정상 종료 회수 세부 정책은 미결정.
- 작업자 수 입력·상한, 비정상 종료 시 복구·중단, 부모·자식 종료·회수.
- epoll LT/ET, 이벤트당 처리 예산, 연결 상한과 과부하 처리.
- 버퍼 상한·역압력·절대 기한과 무진행 타임아웃의 이벤트 루프 적용.
- 정적 파일의 일반 파일 I/O와 CGI 실행·파이프·자식 회수가 이벤트 루프를 막지 않도록 하는 범위·방식.
- Linux/WSL epoll 구현의 이식성 범위. macOS 지원을 묵시적으로 보장하지 않는다.

## 전체 흐름 (설계 초안, 코드 미구현)
main → 설정/리스너 → 고정 작업자 시작 → 작업자 이벤트 루프 → 연결별 상태 재개 → 파싱/라우팅 → 로컬 응답 또는 백엔드 연결/송수신 → 응답 송신 → 자원 회수.
기존 완료 대기 함수 handle_client/proxy_prepare_response/send_prepared_response를 그대로 이벤트 루프에서 호출하지 않는다. 재개 가능한 상태와 준비 이벤트로 전환한다. 구체적인 상태·소유권 변경은 선택 후 문서화한다.

## 검증과 측정
Issue #5 완료 조건과 기존 기능 회귀를 검증 후보로 기록한다. 공식 실행 범위는 구현 후 확정한다. 컴파일과 테스트 결과를 구분한다. 10,000 동시 연결은 검토 목표이며 보장하지 않는다. 성능은 요청률·크기·백엔드 지연·CPU·메모리·FD·반복 조건을 제시하고 별도 허락 후 측정한다.

## 조회 분배 비교 계획

iteration-5-distribution-comparison.md 참조. 후보의 구체 동작 합의 → 구현 → 별도 승인한 조건의 비교 → 사용자 최종 선택 순서다. 이번 결정은 측정 실행 허락이 아니다.

## 공유 메모리·작업자 직접 선택 확정

부모는 accept/IPC FD 전달/작업자 관리·회수, 작업자는 요청 처리/선택/중계/완료·실패 갱신을 맡는다. 선택과 진행 수 증가는 같은 짧은 동기화 구간이며 네트워크·파일 I/O는 잠금 밖이다. 네 정책 선택 실행은 허용하되 최종 채택·자동 전환은 아니다. 세부 확정/제안·main 흐름·FD 소유권은 iteration-5-shared-state-design.md 참조. 제안은 승인 전 구현하지 않는다.

## 1-A/2-A 확정과 복구 검토

익명 공유 mmap + process-shared robust mutex와 복구 후 계속 운영 목표를 확정. 다섯 장애 상황과 아직 선택하지 않은 자료구조·오류 대응은 iteration-5-recovery-review.md 참조. 단일 프로세스 API 초기화 지원 probe만 컴파일/실행 성공했으며 다중 프로세스·사망 복구·제품 회귀·성능 검증은 아니다.

## 구조 A와 복구 범위 채택 / 초기화 첫 구현 (2026-11-25)
사용자는 ①~④ 복구·재복구와 상한 있는 원장/복구 기록, ⑤ 제한 진단 로그 및 신뢰 불가 시 전체 안전 종료, 구조 A를 승인했다. 개별 수정 단계·복구 중 종료 주입 검증도 승인했다. 앞선 제안/선택 대기 문구는 이 결정으로 대체하되 통계·알고리즘·재시작·실행 상한 수치는 아직 미결정.
공유 상태 크기 계획/생성/해제와 독립 단위 테스트를 구현했다. 컴파일 성공 및 ASan/UBSan 단위 실행 성공: 예산 경계/초기화/실제 fork 자식 잠금 보유 중 SIGKILL 후 EOWNERDEAD 감지와 mutex 재사용. 실제 원장 갱신 복구가 통과한 것은 아니다. 상세 함수 리뷰·정확 메모리 예시·미검증 범위는 iteration-5-shared-state-initialization.md 참조. main 통합·전체 복구·로그/전체 종료·성능은 미구현/미실행.

## 원장 commit/rollback·재복구 단위 완료 (2026-11-25)
src/backend_reservations.c의 예약/완료·세대·회수·trylock/owner-death 복구와 bounded diagnostic을 구현했다. 고정 undo1개128bytes, header232/worker40/reservation32, scratch24W bytes/프로세스. 메모리 예시·actual 함수 순서·불변 조건·한계는 iteration-5-reservations-review.md 참조.
make test 10개 통과, 추가 commit 보존 경계를 포함한 최종 새 테스트도 통과: 변경8+8·generation6·재복구7+6·회수20·손상6+3·4프로세스 강제경합. 테스트는 실제 fork/SIGKILL이지만 원장 단위이며 TCP 서버 worker E2E가 아니다. main/epoll·부모 전체 종료는 미통합. 정책/시간 통계·실행 상한·재시작·부하/성능 미결정·미실행.

## 부모·작업자 IPC 다음 설계 단위

iteration-5-ipc-design.md에 main부터 FD 인계·fatal 통지 흐름과 전송/종료/FD 회수 검증 후보를 기록. socketpair SEQPACKET/STREAM, worker 배분, 인계 기준·큐·포화·fatal 우선순위/종료는 아직 선택 대기. 제품 코드·서버 실행·성능 변경 없음.

## IPC STREAM 채택

작업자별 SOCK_STREAM 채널을 선택했다. 부분 sendmsg의 FD 중복 전송 방지·프레임 경계/ancillary 귀속·미완료 프레임 EOF 정리 검증을 후보로 반영. worker 배분·상한·인계·종료 정책은 미정. iteration-5-ipc-design.md의 최신 결정 우선.

## 사용자 확정 — worker에도 네 정책 선택 지원 (2026-11-25)
사용자 1번: worker 분배에도 RR / Least Outstanding+동률RR / 응답시간+동률RR / 진행수·시간 결합+동률RR 네 후보를 선택 실행할 수 있게 구현한다. backend와 후보 비교·점수·동률 계산을 공유하며 불필요한 범용 프레임워크는 만들지 않는다.
worker/backend 상태·순번·정책 설정·시간 측정·표본·실패 반영은 별도 관리한다. 양쪽에 같은 알고리즘을 강제하거나 최종 알고리즘을 확정한 것이 아니다. 기존 parent accept/FD 전달·worker backend 직접 선택·SOCK_STREAM 결정 유지.
worker의 outstanding 기준(미종료 연결/HTTP 처리중 요청), 시작·종료 시점, 부모 통보·세대별 회수, 시간 구간·평균·초기값·노후화·점수·동률·실패 반영은 미결정. backend 통계의 의미를 worker에 자동 적용하지 않는다.
worker/backend 정책을 동시에 바꾼 결과를 한 알고리즘의 효과로 해석하지 않도록 비교 계획에서 다른 층의 정책을 고정하는 조건을 먼저 제시한다. 조합 비교·부하·성능 실행은 별도 허락 필요. 이번에는 코드·검증·성능을 실행하지 않았다.

## worker outstanding 기준 확정 — 사용자 1번 (2026-11-25)
worker 정책의 진행 수는 배정 후 아직 종료되지 않은 클라이언트 연결 수로 한다. 헤더/본문 수신 중·백엔드 대기·응답 전송/마무리 중 연결을 포함하고, 실제 HTTP 처리 중 요청 수만 세는 안은 미채택이다. backend outstanding 시작/종료 시점까지 확정한 것은 아니다.
parent가 배정 상태를 보유하고 worker의 연결 종료 통보로 반영하는 방향. 정확한 배정 증가 시점/미전달 취소·worker 사망·중복/늦은 종료 통보 처리와 queue 상한·ACK는 여전히 미정이다. 연결 ID+worker generation을 기준으로 중복 감소/새 세대 오염 방지를 설계한다. worker/backend 측정값은 별도이며 정책도 최종 미확정.

### 다음 FD 인계 선택안 (미확정)
1 추천: STREAM의 FD 포함 프레임 전체가 전송되면 parent 원본 FD close, 수신 ACK는 요구하지 않음. 첫 양수 sendmsg에 SCM_RIGHTS는 이미 전달될 수 있으므로 fd_sent flag를 두고 나머지 바이트에는 ancillary를 재전송하지 않음. parent는 whole-frame 완료까지 원본을 보유하되 client I/O는 하지 않음. worker는 프레임 전체 검증 전 client 처리 시작 금지 제안.
2 대안: 프레임 전체 전송 후에도 worker 수신/등록 ACK까지 parent 원본 보유. 등록 여부를 명시 확인할 수 있지만 추가 IPC·ACK 대기 원장·FD/기한 비용이 필요. ACK는 HTTP 완료나 worker 생존 보장이 아니며, 성공 전송 후 ACK 소실을 이유로 다른 worker에 재배분하면 중복 처리 위험이 있다.
두 안 모두 첫 FD 전송 양수 성공 이후 같은 연결을 자동 재배분하지 않는 안을 함께 제안한다. 전체 프레임 미완료 상태의 채널 단절/worker 종료 시 각 보유/수신/queued 참조를 정리해야 한다. 위 내용은 선택안이며 아직 구현하지 않는다.

## FD 인계 확정 — 사용자 1번 (2026-11-25)
부모는 FD+연결 정보 전체 프레임 전송 완료 시 원본 FD를 닫는다. 수신/등록 ACK는 요구하지 않는다. worker는 프레임 전체 검증 전 client 처리를 시작하지 않는다. 첫 양수 sendmsg 뒤에는 FD를 다른 worker로 자동 재배분하지 않는다. 부분 송신 후 재개에는 동일 ancillary를 재송신하지 않는다. ACK는 미구현/미사용.
첫 구현 흐름은 큐·정책·worker 수와 독립인 fixed-size STREAM FD 채널이다. frame48bytes: magic/version/type/worker index/generation/connection ID/accept시각/예약0. 모두 고정폭 명시 직렬화, C struct padding이나 요청 데이터를 전송하지 않는다. 첫 바이트 recvmsg를 따로 읽어 SCM_RIGHTS 귀속을 고정하고, 이후 현재 frame의 남은 길이 이상 읽지 않는다. 한 channel의 송신 frame은 interleave하지 않는다.
receiver는 단일 수신 FD만 소유하며 잘못된 frame/ancillary/부분 EOF/이전 세대이면 받은 FD를 정리하고 channel 실패를 반환한다. 실제 channel close·parent 전체 종료·worker생성/배분/큐/포화/시간제한은 후속 흐름. 이 구현이 전체 동시성 서버 완료를 의미하지 않는다.

## STREAM FD 전송 단위 구현·검증 완료 (2026-11-25)
src/worker_fd_channel.h/c 구현, 전체 frame 송신 후 원본 close/noACK, partial send의 FD1회·frame 검증 전 private 보유·오류/EOF FD 회수. 실제 fork/수신 전·부분 수신 후 SIGKILL·MSG_CTRUNC·실제 EAGAIN·FD0/FDcount 검증 통과. 일반 컴파일 성공, make test ASan/UBSan 11개 통과. 실물 함수 순서/소유권/한계는 iteration-5-ipc-transfer-review.md 참조.
main·운영 worker/queue/epoll·공통 네정책·fatal 제어 통지/전체 종료는 미통합. 서버 동시성 완료/부하·성능 통과로 표현하지 않는다. 운영 상한·미결정 정책 자동 채택 없음.

## 전역 FIFO 확정 — 사용자 1번 (2026-11-25)
parent 전역 FIFO에는 worker 미배정 연결만 보관한다. 전송 시작 시 전달 가능한 worker를 공통 정책으로 선택하고 그 worker의 미종료 연결 수를 증가시킨다. worker별 대기 FIFO는 만들지 않는다. channel별 active 송신 frame1개는 기존 STREAM 직렬화 계약을 유지한다.
전역 accepted 미종료 수는 accept 후 관리 장부에 등록할 때 증가하는 안, worker count는 FIFO에서 선택 worker의 SENDING으로 이동할 때 증가한다. FIFO→SENDING→SENT/worker처리 이동은 전역 연결 수를 중복 증가시키지 않는다. 정상 종료/미전달 취소/worker 사망의 정확1회 해제는 별도 장부 구현으로 검증한다.
이미 FD를 보낸 연결 재분배 금지 유지. Q/W/C/G 숫자·포화 동작·대기 기한·설정 입력·fatal 종료 통지는 이번 선택에 포함하지 않는다.

## 초기 상한 묶음 제안 (미확정)
- W=4 workers, Q=128 미배정 FIFO, C=256 worker별 미종료 연결, G=1152 전체 accepted 미종료 연결(128+4*256).
- shared backend reservation R=1024(4*256), mapping byte budget=1MiB. 예산은 허용 한도이며 1MiB 전체를 mmap/선할당하지 않는다. 실제 크기 계산 후 필요한 길이만 매핑.
- 기능 구현 출발점의 임시값이며 최적값·성능 결과·운영 목표가 아니다.1만 연결 검토 목표는 유지하고 상한 변경·측정은 별도 합의한다. 모든 값·입력/관계 검증은 사용자 선택 후 구현한다.
- 현재 WSL ulimit -Sn=10240, -Hn=1048576 확인. 조회값만 읽었고 한도/OS 설정을 변경하지 않았다. 프로세스별 한도이며 시스템 전체 FD 예산과 같지 않음.
- parent client 원본 FD 최대Q+W=132 제안(추가 listener/IPC/epoll/시그널/관리 FD 제외). 순수proxy worker의 client+backend FD는 최대2C=512 산술 예시. 정적 file/CGI pipe/child·기타 FD와 전체 시스템 자원은 별도 점검.
- 현재 shared layout W4/R1024는232+40*4+32*1024=33160bytes≈32.4KiB(미결정 시간 통계 필드 제외), local validation scratch=96bytes/프로세스. 시간 통계/추가 장부 도입 후 산식 재확인.
- 동시에 배정된1024 연결 모두 header8KiB+body64KiB를 보유한다는 가정은72MiB. relay/응답 buffer·FD/소켓 kernel buffer·프로세스/CGI/parent 장부는 미포함. 서버 전체 메모리 상한 보장 아님.
이번 기록은 제안이며 설정/큐 코드·검증·성능을 실행하지 않았다. 포화 때 accept 보류/즉시 거절은 다음 선택이다.

## 초기 상한 묶음 채택 — 사용자 1번 (2026-11-25)
W=4 고정 프로세스 작업자, Q=128 parent 미배정 전역 FIFO, C=256 worker별 배정 후 미종료 연결, G=1152 전체 accepted 미종료 연결, R=1024 공유 backend 예약 슬롯, 공유 mapping budget=1048576bytes(1MiB)를 초기 임시값으로 채택했다. 최적값/성능 측정 결과가 아니다.
G=Q+W*C, R=W*C. 미배정 FIFO는 worker outstanding에 포함하지 않지만 전체 accepted 수에는 포함한다. SENDING/SENT/worker 처리로 이동해도 같은 연결을 중복 계산하지 않는다. partial sender는 worker assigned 상태의 일부다.
parent 보유 client 원본 FD 최대Q+W=132, 현재 shared layout33160bytes≈32.4KiB, validation scratch96bytes/프로세스라는 앞선 산식은 현재 타입 기준 예산 설명이며 실제 전체 메모리/RSS 상한이 아니다. budget1MiB를 전부 선할당하지 않고 필요한 길이만 mmap. 통계/다른 구조 추가 후 산식 재계산·budget 검사.
기존1만 연결 검토 목표를 대체하지 않으며 더 큰 상한 적용/비교는 별도 합의. worker/backend 네 알고리즘은 최종 미확정. 큐 포화 처리·대기/partial 기한·실행 설정 입력·epoll 모드·fatal 종료/재시작은 이번 수치 승인에 포함하지 않는다.
아직 코드의 실행값을 설정하거나 main에 적용하지 않았다. 포화 동작 선택 후 합의된 parent queue/control 흐름을 구현한다. 부하/성능 실행 승인 아님.

## 포화 동작 확정 — 사용자 1번 (2026-11-25)
Q 또는 G 상한에 도달하면 새 accept를 보류, 여유가 생기면 재개. 기존 client/IPC/종료 처리는 계속. HTTP503/accept후즉시닫기는 추가하지 않는다. kernel backlog의 대기/연결 실패 가능성과 app queue의 구분 유지.
첫 구현 단위: 고정 배열 ring FIFO의 생성/이동/해제와 parent_accept_has_capacity. FIFO는 parent만 소유하고 thread/shared mutex를 사용하지 않는다. accepted FD/ID/accept시각만 저장, parent HTTP 파싱/worker선택은 수행하지 않는다. 상한이 차면 enqueue 자체는 ENOSPC/FD 소유권 유지, 실제 parent loop는 accept 전 gate를 확인해 그 상황을 예방한다.
초기 W4/Q128/C256/G1152/R1024/1MiB는 concurrency_limits.h 한 곳에 기록한다. 현재 main 실행값/CLI에 적용하지는 않는다. parent 전체 연결 장부/worker 종료통보/epoll 등록·해제는 후속 통합이며 gate가 존재한다고 서버가 이미 포화 보류 동작을 수행한다고 주장하지 않는다.

## parent FIFO/gate 단위 완료 (2026-11-25)
concurrency_limits.h의 임시 상한6개, parent_client_queue.h/c의 고정ring FIFO·FD이동/정리·queue/global/stopping gate 구현. 일반 컴파일/ASan·UBSan unit 성공, make test12개 통과. Q128wrap/full·G경계·FD count/cleanup 및 실제loopback 테스트driver의 accept보류/재개·STREAM연결 검사 통과. 상세 iteration-5-parent-queue-review.md.
main/실제epoll·전체연결장부/workerC256/종료통보·fatal전체종료는 미통합. FIFO4128bytes와gate는 전체서버자원/동시성보장이 아니다. 성능/서버E2E 미실행.

## epoll LT 채택 (2026-11-25)

사용자1번으로 Level-Triggered 확정, ET/ONESHOT 미채택. 별도긴급pipe/진단슬롯 vs기존STREAM재사용 제어통지는 선택 대기. 비용/범위/검증 후보는 iteration-5-control-notification-design.md 참조. 이번에는 문서만 변경, 코드·검증·성능 실행 없음.

## 긴급 경로 채택 — 사용자 1번 (2026-11-25)
일반 CLOSED는 기존 STREAM worker→parent, 긴급 알림은 전역 nonblocking pipe1쌍+worker별 고정 진단칸을 채택. client FD STREAM/noACK는 유지. 고정 진단은 reporter와대상worker/세대·검사/phase/stage·종료이유 숫자만, 가변문자열/body/headers/URI 없음.
첫 단위는 공유slot 초기화/arming·원자게시·읽기·boundedformat과pipe 생성/알림/소비. slot은 worker당1개(상한W), firstreport를 같은세대에서 덮어쓰지 않음. parent는 초기fork전 또는 waitpid로종료확인후만 다음세대를arm, 자동재시작은하지않음. slot publication EMPTY→WRITING→READY, ready전payload를읽지않음. 게시도중사망은 incomplete/unknown으로분류하고원인을추정하지않음.
parent의실제SIGCHLD/epoll 대기경합·로그sink적체/강제종료기한·전체종료는후속통합. 특히 게시후pipe쓰기전사망은pipe알림이없을수있어SIGCHLD 경로가필요함. 이단위가그전체wake/종료보장을완성했다고주장하지않는다.

## 긴급pipe·고정진단 단위 완료 (2026-11-25)
worker_failure_channel.h/c와version3 sharedslot 추가. fatal의도→WRITING→READY 게시, firstreport유지·generation차단·READY전payload미조회, 별도nonblocking pipe알림/소비와제한숫자/unknownformat 구현. slot64bytes·W4추가256, 초기mapping33416bytes/budget1MiB유지.
일반컴파일성공, make testASan/UBSan13개통과. 실제LT/pipe·STREAM포화/EPIPE·5경계SIGKILL/다중게시/손상/복구불가mutex·FD회수검증. 상세 iteration-5-failure-channel-review.md. main/SIGCHLD/실제전체종료·로그sink적체/기한은미통합이며특히알림전사망은SIGCHLD경로가필요하다. 서버E2E/성능미실행.

## 시그널·epoll 경합 다음 선택안

epoll_pwait+최소 handler / signalfd의 비용·main 흐름·fork/CGI 회수 경계를 iteration-5-signal-design.md에 기록했다. 현재 선택 대기이며 코드·검증·성능을 실행하지 않았다.

## 사용자 확정 — 1번 (2026-11-25)
epoll_pwait + 최소 sig_atomic_t handler를 채택했다. signalfd/시그널전용FD는 추가하지 않는다. 모듈은 caller가 보유한 제어 상태와 OS handler를 받아 새 가변 전역 상태를 만들지 않는다. handler는 종료0/실행1/자식변경을포함한실행3만 남기고, 종료 뒤SIGCHLD가 와도실행으로 되돌리지 않는다. 관리handler의sa_mask에관리시그널전체를넣어중첩갱신을막는다.
첫단위는mask/disposition 설치·복원, epoll_pwait, child-event표시 소비와exec자식mask/default복원이다. 실제waitpid는하지않아부모worker/작업자CGI단일회수책임을유지한다. main/기존blockingCGI는아직연결하지않는다. caller는대기전상태를확인하고EINTR을무조건재시도하지말아야한다.

## 시그널 첫 실행 실패·보강 선택 대기 (2026-11-25)
process_signal_wait.h/c와새단위를구현/컴파일. pending/대기직전6개통과, 항상readable LT I/O에서pending종료우선반례실패. 이후시그널시험미실행, 새단위포함전체make test는통과상태가아니다. main미통합/성능미실행.
원인·원문·두수정안과예상syscall비용은 iteration-5-signal-review.md 참조. wait반환후잠깐unmask/reblock(추천) vs pending조회/0초소비는사용자선택전구현하지않는다. 기존epoll_pwait선택·최소handler·HTTP정책을임의변경하지않음.

## 연결 장부·CLOSED 단위 완료 (2026-11-25)
사용자 1번을 parent_connection_ledger.h/c 및 worker_closed_channel.h/c로 구현했다. QUEUED/SENDING/OWNED와 early CLOSED, 토큰 재사용 세대, 정확1회 해제·waitpid 이후 회수, bounded worker 큐와32byte STREAM 부분 I/O를 제공한다. 장부/완료 큐는 FD를 소유하지 않는다. 실제 W4/C256/G1152 경계·부분 I/O/실제EAGAIN·비정상 ancillary·3경계 SIGKILL 회수 포함 ASan/UBSan make test 15개 통과. 일반O2 컴파일 성공. 장부+완료큐+수신 상태62,104bytes(현재ABI), 다른서버자원 제외. main/실제epoll/서버E2E·성능 미통합/미실행. 상세 iteration-5-connection-completion-review.md. 이전 ‘제안/미선택’ 기록은 이 선택과 결과로 대체한다.

## 부모 감독 단위 완료 (2026-11-25)
parent_worker_supervisor.h/c에 PID별 WNOHANG·격리·임시2000ms 유예/TERM/KILL 각1회·waitpid 확인 후 sender/채널→연결장부→backend 원장 회수를 구현했다. shared EBUSY는pending, 신뢰불가면전체종료, 유효worker0 오류종료, 자동대체없음. 첫숫자진단고정보존·ECHILD시PID신호/파괴차단. 일반모듈O2와make all성공, 실제fork/신호/잠금경합 포함ASan/UBSan make test16개통과. W4추가560bytes(현재ABI), 성능미측정. 상세 iteration-5-supervisor-review.md. main/실제HTTP worker·log sink·부모사망·정책점수·EBUSY 재시도 wake/간격은미통합/미결정이며 테스트의20ms를운영정책으로채택하지않음. make verify/부하/커밋/머지/푸시없음.

## 10ms 제어 재시도 완료 (2026-11-25)
사용자1번을 worker별 WAIT/BACKEND retry_kind/retry_at_ms와INITIAL_SUPERVISOR_RETRY_MS=10으로 구현했다. 기한전같은작업보류·due시1회·pending해제·sharedfatal우선, parent_supervisor_wait_timeout_ms로종료/재시도최소기한 제공. 추가요청없는실제epoll타이머회수·ready I/O trylock제한·worker별기한·새CHLD없는EINTR재회수 통과. 일반O2/make all성공, ASan/UBSan전체16개통과. W4감독저장592bytes(이전보다32증가). 상세 iteration-5-supervisor-retry-review.md. main/HTTP worker미통합, E2E/성능미실행. 이전재시도방식미결정문구는이번선택으로대체.

## backend lifetime 단위 완료 (2026-11-25)
backend_request_lifetime.h/c에begin_locked→ACTIVE→mark_finished→RELEASE_PENDING→try_release→RELEASED/reset구현. 공유선택과예약동일잠금·EBUSY토큰보존·중복첫outcome유지·재사용보호. 실제fork경합/3경계SIGKILL·parent회수·잔여client출력/worker수분리·half-close socketpair시험포함ASan/UBSan전체17개통과, 일반O2/make all성공. 객체32bytes(현재ABI). 실제HTTP프레이밍완료hook/worker상태머신·release재시도미통합/미결정;기존parser회귀와구분. 성능/E2E/커밋/머지/푸시없음. 상세 iteration-5-backend-lifetime-review.md.

## 네 분배 정책 구현·단위 검증 완료 (2026-11-25)
공통 distribution_policy(네정책/EWMA1/8/32회탐색/실패floor1초/포화곱/정확동률), sharedv4 통계+선택예약/완료해제 transaction과복구checksum, backend고정시간구간, parent관측CLOSEDv2(56byte)/선택배정, 필수worker-policy/query-policy설정 구현. 순차query경로도정책/표본연결;worker-policy의실제multiworker main연결은아직미통합. 예제config/proxy.conf.example 추가, 개인build설정불변. 일반O2/make all·ASan/UBSan전체18개통과. 통계변경10·선택변경8경계SIGKILL/재복구/2프로세스경합 검증. sharedmapping33528bytes(W4/R1024),상세비용/함수흐름 iteration-5-policy-review.md. 제품make verify/E2E·부하/성능·커밋/머지/푸시없음. 최종알고리즘미정유지.

## 실제 worker nonblocking 네트워크 단위 완료 (2026-11-25)
worker_http_io.h/c·worker_http_internal.h·worker_proxy_io.c에기존FD/CLOSED채널+실제LT epoll다중연결·요청/100·backend선택/connect/송신/헤더/본문·16/8KiB역압력·worker10ms/선택1초503·해제pending·전체/idle기한·예산·세대eventtoken구현. 기존pureheader/parser재사용. FD전달중증거있는TCPreset은CLIENT_GONE으로연결취소처리,다음frame/FD회수검증. strictmake all/worker-io및ASan/UBSan전체20개통과,실제fork/TCP/Cbackend/4MiB binary/느린A중B·제출진전/네정책/SIGTERM대기중회수확인. 상세 iteration-5-worker-io-review.md. 제품main은여전히순차,localfile/CGI/부모실행통합/긴급통지최종조율미완료. make verify제품E2E·부하/성능·커밋/머지/푸시없음.

## 구현 우선 운영 변경 (2026-11-25)
사용자 지시: 확정한 흐름을 이어 구현한다. 중간 확인은 compile/link와 변경에 직접 필요한 최소 기능(정상,느린/빠른동시처리,종료·자원정리)로 제한한다. 전체회귀/광범위사망주입은 실행가능 통합 체크포인트에 모은다. 실패는보고, 성능은별도승인이다.
이번 단위는 parent의실제listener LT·FIFO/분배·부분FD/CLOSED·supervisor·fatalpipe를한루프로연결한다. localfile/CGI의새 I/O선택은미정이므로제품 main전환은아직하지않고 실제fork4worker와Cbackend를사용하는짧은통합실행에서검증한다. 기존main경로의기능을삭제/블로킹worker로우회하지않는다.

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
