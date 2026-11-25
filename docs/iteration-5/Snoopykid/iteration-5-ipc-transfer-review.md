# Iteration 5 STREAM FD 인계 — 실물 코드 리뷰와 결과

2026-11-25. 채택: SOCK_STREAM, worker/backend 네 후보의 선택 계산 공유(상태/설정은 별도), worker outstanding=배정 후 미종료 연결, 전체 frame 전송 후 parent 원본 close/no ACK. 이 보고서는 FD 채널 단위이며 전체 동시성 서버 완료 보고가 아니다.

## 실제 구현 파일
src/worker_fd_channel.h/c, tests/unit/test_worker_fd_channel.c, Makefile 독립 test_worker_fd_channel 타깃. 기존 main/SRCS/HTTP 로직은 아직 연결하지 않았다. 모든 바이너리/로그는 ignored build 아래.

## main부터의 위치
다음 통합 흐름: main → create_worker_fd_channel(worker별) → fork → 불필요한 IPC/listener/client 사본 정리 → parent accept/metadata/worker 선택 → prepare_worker_fd_transfer → event loop에서 advance_worker_fd_send → worker event loop에서 advance_worker_fd_receive → 완료된 client를 HTTP 상태 머신에 넘김.
현재 main은 여전히 순차 서버이고 위 호출이 들어가지 않았다. queue/worker수/epoll LT·ET·공통 정책·fatal 제어 통지/전체 종료는 다음 선택/구현 대상.

## 실제 함수 순서
1. create_worker_fd_channel: AF_UNIX/SOCK_STREAM socketpair + NONBLOCK/CLOEXEC. 생성된 양끝 FD 소유권은 호출자에게 있다. 내부는 현재 Linux/WSL 플래그 사용이며 macOS 호환을 보장하지 않는다.
2. prepare_worker_fd_transfer: caller의 client FD와 metadata 검증 → 고정 frame 직렬화 → sender가 FD 소유, caller 변수는 -1. 준비 실패는 caller 소유권 그대로, 진행 중 sender 덮어쓰기 금지. client는 연결된 stream socket이어야 함(regular file/미연결/listener를 client로 처리하지 않음).
3. advance_worker_fd_send: 1호출 최대1번 nonblocking sendmsg. 최초 양수 송신에 SCM_RIGHTS가 전달되고 fd_sent=true. EAGAIN/EINTR이면 현재 offset/소유권을 보존하고 즉시 PENDING. 후속 바이트에는 ancillary를 다시 붙이지 않음.48bytes 전체 송신 완료 시 parent 원본 close, sender owns=false/finished=true, READY. 수신 ACK/등록 보장 아님.
4. advance_worker_fd_receive: first recvmsg는1byte만 읽어 FD 귀속 고정, FD는 receiver가 보관. 이후 현재 frame 남은 길이 이내만 recvmsg하므로 다음 frame의 anchor/FD를 소비하지 않음. 입력0/진행중/PENDING에서는 client를 외부로 공개하지 않음.
5. extract_received_fd: 첫 바이트에 FD1개만 허용, body 단계의 추가 FD/알 수 없는 control/복수/MSG_CTRUNC·TRUNC 거절. 받은 FD들은 모두 close. Linux가 control 용량 초과분을 자동 close하는 경로는 실제32FD 주입으로 관측/누수 검사했다.
6. decode_client_frame: magic/version/type/예약0·ID/세대/timestamp와 expected worker/generation 검증. getpeername/SO_TYPE 확인, O_NONBLOCK 설정. FD_CLOEXEC는 recvmsg(MSG_CMSG_CLOEXEC)에서 적용. 전체 검증 후에만 received_client로 FD 소유권 이동, receiver는 빈 다음 frame 상태로 복귀.
7. abort_worker_fd_send/receive: 아직 소유한 FD만 한 번 close. 전송 중 fatal/부분 EOF도 이 정리 경로 사용. failed 상태는 resync하지 않으며 caller가 channel을 닫아야 한다. 이미 FD가 전송된 연결을 다른 worker로 재배분하지 않는다. 인자 오류/EBUSY는 caller가 소유 중인 output을 덮어쓰지 않는다.

## 소유권 타임라인
- prepare 전: caller(parent queue) 소유.
- prepare 성공: sender 소유, 원본 caller 변수 -1.
- 첫 양수 sendmsg: kernel/receiver 참조가 생길 수 있지만 sender 원본은 유지. parent는 client read/write하지 않음.
- receiver first byte: receiver private FD, 아직 HTTP 처리에 넘기지 않음.
- 전체 송신: sender 원본 close. 전체 수신·검증: worker caller가 FD 소유.
- frame 중간 단절: sender 원본/receiver 보관 FD를 각 소유자가 정리, caller가 해당 IPC channel 종료. worker 사망 시 커널 queued/received 참조 정리.
close(EINTR)는 재시도하지 않는다. 전체 frame 이후 close가 실패해도 FD는 이미 전달됐을 수 있고 finished/fd_sent를 유지한다. 그 결과를 미전송으로 보고 재전달하면 안 된다.
한 channel에서 보내는 frame은 직렬화해야 한다. 서로 다른 sender의 frame 바이트를 교차시키면 복구/재동기화를 시도하지 않고 channel을 실패시킨다. 부모가 각 worker 채널에 보류 frame1개를 관리하는 통합 계약이 필요하다.

## 고정 frame과 메모리
wire48bytes: magic/version/type/worker index(각4), worker generation/connection ID/accepted seconds(각8), nanoseconds/reserved(각4), 명시 big-endian 직렬화. C struct padding/요청 본문/URI/헤더/민감 입력 전송 없음. accepted_at은 값 그대로 전달하며 header deadline을 IPC 단계에서 새로 시작하지 않는다.
송수신 상태 각각 frame48bytes+offset+FD+소유 플래그의 고정 크기, heap 할당/확장 없음. syscall ancillary 임시 공간은 FD 송신1개, 수신 최대8개 검사 공간으로 한정한다(전달 허용량은1개). 운영 queue/동시 연결·worker 수·전체 FD 예산은 아직 미결정이고 이 숫자들이 운영 상한을 대신하지 않는다.

## 컴파일 확인
hook 없는 -std=c11 -Wall -Wextra -Werror -O2 worker_fd_channel.o 컴파일 성공.
make build/test_worker_fd_channel: ASan/UBSan 컴파일/링크 성공. WORKER_FD_CHANNEL_TEST wrapper는 테스트에서만 실제 syscall의 길이/일시 오류를 제어하며 제품 빌드에 hook/변경 가능한 전역 상태 없음.

## 실제 실행 결과
최종 make test 11개 바이너리 통과, failure/skip0, sanitizer 진단 없음. 로그 build/iteration5-capability/unit-tests-with-ipc.log.
새 단위 10개 함수 그룹:
- STREAM/NONBLOCK/CLOEXEC, invalid metadata·미연결 socket 준비 거절/소유권 유지.
- 실제 양수 short send/receive를1byte로 제한, EINTR/EAGAIN 주입, 최초 FD1회, 전체 송신 전 원본 유지/완료 close, 전체 수신 전 output 미공개, flags/메타데이터 보존.
- 테스트 채널 SO_SNDBUF4096으로 실제 EAGAIN까지 채움→미전송 소유권 유지→drain→정상 재개. 실제 서버/OS전역 설정은 바꾸지 않음.
- 두 frame을 먼저 송신 후 수신해 각 FD와 metadata 귀속 확인, clean EOF 구분.
- malformed frame10종, FD누락/2개/32개 control, 실제 MSG_CTRUNC 관측과 전달 FD 모두 정리.
- body 단계 늦은 FD, regular file 전달 거절.
- partial EOF/반복 cancel/FD번호 재사용 후 이중 close 없음, peer close의 SIGPIPE 방지/원본 정리.
- 실제 fork, inherited client 사본 모두 닫은 뒤 받은 FD0으로 통신·flags 검증.
- inherited source 사본 정리 확인 후 recv 전 SIGKILL: kernel queued FD 참조 해제, monitor peer EOF.
- first anchor/private FD 수신 후 SIGKILL: sender 후속 EPIPE/원본 정리, monitor peer EOF, fd_sent 유지해 재전달 금지 판단 가능.
정상/오류·fork 종료 흐름 후 /proc/self/fd 개수 복귀 확인. 원장 변경·재복구 기존 테스트도 함께 통과.

## 검증 한계·다음 단위
실제 테스트는 standalone socketpair/fork이며 main 기반 TCP worker E2E가 아니다. parent queue/overload·worker 생성/배분·타임아웃·partial frame 장기 점유·fatal 우선순위/전체 종료는 미구현. socketpair/fcntl/close의 실패 주입·unknown ancillary·socket fd 정체 위조의 모든 조합·다른 OS는 미검증. 실제 POSIX 시그널로 syscall EINTR을 재현한 것이 아니라 test wrapper 분기 검사다.
임시 테스트 소켓 외 실제 정적 루트·권한·proxy.conf는 변경하지 않았다. make verify 서버 E2E/부하·성능 측정은 실행하지 않았다. 테스트 alarm/harness/poll 대기는 운영 기한이나 측정 조건으로 채택하지 않는다. commit/merge/push 없음.
