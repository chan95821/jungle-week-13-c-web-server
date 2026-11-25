# 부모·작업자 IPC — 구현 전 선택안

2026-11-25. 부모 accept/FD 전달과 작업자 직접 backend 선택은 확정. 아래 IPC/dispatch/인계·종료 방식은 아직 제안이며 자동 채택하지 않는다.

## main부터 연결할 흐름 제안
main → 설정/공유 영역 → create_worker_channel → fork → 부모/자식의 불필요한 IPC·listener 사본 close.
부모 run_parent_loop → accept → 연결 ID/accepted_at 기록 → select_worker → dispatch_client_fd(sendmsg/SCM_RIGHTS).
작업자 run_worker_loop → receive_client_fd(recvmsg) → 제어 메시지·FD 개수·세대 검증 → 연결 상태/epoll 등록 → nonblocking HTTP 상태 머신.
worker 공유 원장 fatal → 잠금 밖 bounded diagnostic 통지 → 부모가 accept 중단/전체 종료. 로그가 막히거나 제어 큐가 차도 안전 종료가 멈추지 않는 수단은 아래 추가 선택이다.
현재 shared module만 구현되어 있고 main은 순차 서버다. IPC를 만들었다고 동시성 구현 완료로 표현하지 않는다.

## 첫 선택: 채널 타입
1 추천: 작업자별 AF_UNIX socketpair(SOCK_SEQPACKET), nonblocking/CLOEXEC. 메시지 경계와 순서가 보존되고 FD와 연결 메타데이터를 한 패킷으로 전달. 현재 Linux/WSL 대상이며 다른 OS 지원 자동 보장 없음. 잘린 packet/ancillary·예상외 FD를 검사하고 수신한 FD를 모두 정리해야 함.
2 대안: AF_UNIX socketpair(SOCK_STREAM), nonblocking/CLOEXEC. stream이므로 부분 송수신/여러 메시지 동반 도착을 누적하고 SCM_RIGHTS가 어느 메시지에 귀속되는지 별도 프레이밍으로 관리. 현재 제어 메시지 용도로 구현/검증 비용이 커질 수 있으나 더 느리다고 단정하지 않음.
FD 전달은 숫자 fd를 공유하는 것이 아니라 같은 open file description에 대한 수신 측 참조를 받는 방식. 원격 프로세스에서 번호가 달라도 정상이다. SCM_RIGHTS 전송 성공은 recv/epoll 등록/HTTP 처리 완료 보장이 아니다.

## 뒤이어 선택할 정책
- worker 배분: 전달 가능한 worker 중 RR(간단, 작업량 균등 보장 없음) / 부모가 관찰한 미종료 연결 수 최소+동률RR(완료 통보·세대별 장부·동기화 비용 추가). 이것은 조회 A/B 분배 알고리즘과 독립이다.
- 인계 기준: sendmsg 성공 때 부모 사본 close / 수신 ACK까지 부모 사본 유지. ACK안도 요청 처리 완료를 보장하지 않으며 성공 전달 이후 재배분하면 중복 읽기/처리 위험이 있으므로 자동 재배분 금지 제안. 추가 전송/상태/FD 보관 비용 비교.
- EAGAIN은 아직 안 보낸 FD를 부모 소유의 상한 있는 큐에 둠. 큐 크기·전체 연결 상한·포화 시 accept 보류/정리·전달 대기 기한은 미정. accepted_at을 그대로 전달해 대기 때문에 기존 헤더 완료 기한이 새로 시작하지 않도록 하는 안 제안.
- fatal 제어 통지는 FD 전달 적체와 별개로 전달 기회를 보장해야 한다. 전용 채널/제어 우선순위·예약 공간/시그널 fallback 및 shared fatal 확인 주기·부모 사망·강제 종료 기한을 비교 후 선택. 단순 send 성공 가정으로 안전 종료를 보장하지 않는다.
- worker 수/실행 설정·epoll LT/ET·등록 실패·프로세스 복구/재시작은 미결정 부분을 다시 선택. 이미 확정한 프로세스+epoll 방향은 재질문하지 않는다.

## 선등록 검증 후보
- 실제 socketpair/fork로 FD 전달, 수신 FD의 소켓 peer·CLOEXEC/nonblocking·accepted_at/ID·worker 세대 확인. 임시 loopback/socketpair만 사용.
- 수신 전/후·send 성공 전/후·epoll 등록 전/후 worker 종료, 남은 사본/커널 queued 참조와 FD 누적·중복 close 검사.
- EAGAIN/채널 EOF/EPIPE/EINTR, 잘린 데이터/MSG_CTRUNC·FD0/여러 개/잘못된 메시지·이전 세대, 예상외 FD 모두 회수.
- ACK 선택 시 중복/이전 세대 ACK·성공 전달 뒤 재배분 없음.
- fatal 전송 큐 적체·수신자 사망·로그 sink 정지에서 승인한 종료 동작, 내용은 숫자/enum만·민감 payload 없음.
- 새 API 컴파일과 실제 기능/장애 실행은 구분. 부하·성능 미승인. 이번 문서는 후보 기록으로 검증 실행이나 정책 확정이 아님.

## API 근거
https://man7.org/linux/man-pages/man7/unix.7.html
https://man7.org/linux/man-pages/man2/recvmsg.2.html

## 채널 타입 확정 — 사용자 2번 (2026-11-25)
작업자별 AF_UNIX socketpair(SOCK_STREAM)으로 확정. SEQPACKET은 비교 대안으로만 보존하며 사용하지 않는다. nonblocking/CLOEXEC·프레이밍·인계 세부는 해당 설계를 확정한 뒤 구현한다. 부모 accept/SCM_RIGHTS 전달·worker 직접 backend 선택은 유지.
STREAM의 한 sendmsg 성공이 메시지 전체 전송을 뜻하지 않는다. 양수 부분 송신에서도 FD는 이미 전달될 수 있으므로, 동일 FD ancillary를 다시 보내지 않도록 전송 오프셋과 FD 전달 여부를 별도 관리하는 설계가 필요하다. 수신은 프레임 경계와 ancillary 귀속을 명시하고, 예상외/잘린 control·EOF에서 받은 FD를 정리한다.
앞선 'sendmsg 성공 때 부모 사본 close'는 STREAM에서는 최초 FD 전송 성공/전체 프레임 완료/ACK 시점으로 세분해 다시 선택받아야 하는 후보이지 확정 정책이 아니다. 어떤 경우에도 전송된 요청을 자동으로 다른 worker에게 재전달하지 않는 원칙은 별도 인계 합의에 포함한다.
현재 worker 선택 RR/최소 미종료 연결, 큐·포화·FD 인계·fatal 통지·전체 종료는 미결정. 이번 결정만으로 구현·검증·성능 측정을 실행하지 않았다.

## 작업자 분배의 백엔드 선택 로직 재사용 요청 (2026-11-25)
사용자는 작업자 분배도 조회 A/B 백엔드 로직을 재사용하는 방향을 요청했다. worker RR을 확정한 것으로 처리하지 않는다.
현재 실제 proxy_select_backend는 순차 RR이며, 새 reserve_backend_request_locked는 선택된 backend와 turn을 원자 예약하는 모듈이다. 네 정책 공통 선택 엔진은 아직 구현하지 않았다.
재사용 경계 제안: 정책별 후보 비교·점수·동률 RR 계산을 작은 함수로 공유. 부모의 worker 상태와 잠금 아래 backend 상태는 별도 보유하고 각각 호출. 부모는 여전히 HTTP 파싱/backend 선택을 하지 않는다. worker 선택 함수가 shared backend mutex/원장/FD 인계를 대신 소유하지 않는다. 범용 plugin/callback 프레임워크로 확대하지 않는다.
예시 흐름(함수명 미확정): parent select_worker → 공통 choose_target, worker select_backend → 같은 choose_target → shared reservation commit. 현재 고정2개 query backend를 가변 N대 지원으로 확장한다는 의미가 아니다.
worker와 backend의 순번·진행 수·응답 시간은 독립: worker 연결 미종료 시간에는 slow client/로컬 파일/CGI/IPC 대기가 포함될 수 있어 backend 측정값을 그대로 재사용하지 않는다. worker용 시간 구간/표본/실패 규칙과 정책 선택 입력은 별도 미정.
범위 확인 대기: worker에도 네 정책 모두 선택 가능하게 할지 / 공통 계산 재사용만 채택하고 worker 정책·확장 범위는 후속 결정할지. 이번 요청을 네 알고리즘 동시 성능 비교 실행이나 동일 정책 강제 선택으로 간주하지 않는다. 코드/테스트/측정 변경 없음.

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
