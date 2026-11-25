# 시그널과 epoll 대기 경합: 선택안

2026-11-25. 기존 코드와 API를 확인한 설계 제안이다. 아직 시그널 방식·코드 변경·테스트 실행을 승인한 기록이 아니다.

## 해결할 문제

플래그를 확인한 직후, epoll 대기에 들어가기 전에 종료·자식 사망 시그널이 처리되면 이미 지나간 시그널로는 새 대기를 깨우지 못할 수 있다. 긴급 진단 게시 후 pipe 쓰기 전에 작업자가 죽는 경로도 있으므로 SIGCHLD 회수가 필요하다. 단순 EINTR 재시도나 긴급 pipe 하나만으로 이 경합을 해결했다고 하지 않는다.

현재 main은 SIGINT/SIGTERM handler에서 g_running만 0으로 만들고, SIGPIPE를 무시한다. SIGCHLD handler/작업자 이벤트 루프는 없다. CGI는 직접 waitpid(pid)를 사용하며 일부 경로는 blocking poll/read/wait다.

## 1. epoll_pwait와 최소 플래그 handler — 추천

일반 이벤트 처리 중 SIGINT/SIGTERM/SIGCHLD를 차단하고 상태·종료 자식을 확인한다. 대기할 때 epoll_pwait가 원자적으로 관리 시그널을 허용해, 대기 직전 시그널을 놓치지 않도록 한다.
handler는 sig_atomic_t 상태만 변경한다. 종료가 자식 이벤트로 되살아나지 않게 종료 우선 규칙과 관리 시그널끼리의 handler 중첩 방지를 구현한다. 추가 연결 상태를 전역에 두지 않고 기존 프로세스 제어 플래그 하나에 종료/자식 변경을 표현하는 방향이다. 코드 이름·인코딩은 구현 시 명확히 한다.
장점: 기존 최소 handler 원칙 유지, 시그널 전용 FD가 필요 없다. 비용: 처리 중/대기 중 마스크, EINTR, fork 상속, CGI exec 전 마스크 복원 순서를 엄격히 관리해야 한다. epoll 계열이므로 현재 Linux/WSL 범위이며 POSIX 전체 이식성 보장은 아니다.

제안 호출 흐름:
main → 관리 시그널 차단/handler 설치 → 공유 상태·IPC·fork → 상태/사망/fatal 확인 → epoll_pwait(LT) → EINTR도 상태를 다시 확인 → waitpid(WNOHANG) 회수 → 종료 또는 I/O 진행.

## 2. signalfd로 시그널도 FD 이벤트 처리

관리 시그널을 차단한 뒤 프로세스별 signalfd를 만들고 LT epoll에 등록한다. read 결과에서 종료/자식 이벤트를 해석하고 실제 자식 회수는 waitpid로 수행한다.
장점: 비동기 handler 대신 정상 흐름으로 이벤트를 읽을 수 있다. 비용: 프로세스별 추가 FD 하나와 등록·읽기·정리, fork/CGI exec 마스크 복원. signalfd는 추가 Linux 전용 API이므로 사용자의 명시적 선택 없이 도입하지 않는다. 더 빠르거나 더 안전하다고 미리 판정하지 않는다.

## 부모·작업자·CGI 회수 경계

부모는 자신이 생성한 작업자 PID/세대를 관리한다. SIGCHLD는 한 번에 여러 자식의 종료를 의미할 수 있으므로 waitpid(WNOHANG)를 필요한 만큼 확인한다. 알림 개수와 실제 종료 수를 같다고 가정하지 않는다.
작업자는 자신의 CGI 자식을 관리한다. 기존 cgi_body의 waitpid 책임을 이벤트 루프가 먼저 빼앗으면 ECHILD/종료 상태 유실이 생길 수 있다. CGI 상태 저장·회수의 단일 소유자를 정하고 blocking body_provider 전환과 함께 구현해야 한다. 이번 시그널 선택만으로 CGI의 blocking I/O가 해결되는 것은 아니다.
선택한 방식과 관계없이 fork 후 불필요한 FD 정리, CGI exec 전 신호 mask/disposition 복원이 필요하다. 작업자 종료 후 CGI 자식의 생명주기·부모 사망 대응·종료 기한·재시작 제한은 별도 결정으로 남긴다.

## 검증 후보

- 시그널 차단 중/상태 확인 직후/대기 진입 직전/대기 중 SIGINT·SIGTERM·SIGCHLD. 대기 영구 정지와 종료 플래그 재활성화 없음.
- 여러 작업자 동시 종료·시그널 병합, 정상 I/O와 종료 동시 준비, 긴급 게시 전/후·pipe 쓰기 전 작업자 사망.
- 실제 waitpid 회수 후 공유 세대/진행 수 정리, 로그에 incomplete/unknown을 유지하고 요청 본문을 남기지 않음.
- fork 자식의 마스크/handler 수명, CGI 종료 상태의 중복 회수 방지. 테스트의 단순 자식과 실제 CGI E2E 범위를 구분한다.
- 컴파일과 실제 동작·장애 실행을 구분하며 시간 제한은 테스트 hang 방지용이다. 부하·성능 실행은 별도 승인.

## API 근거

- https://man7.org/linux/man-pages/man2/epoll_wait.2.html
- https://man7.org/linux/man-pages/man2/signalfd.2.html

## 사용자 확정 — 1번 (2026-11-25)
epoll_pwait + 최소 sig_atomic_t handler를 채택했다. signalfd/시그널전용FD는 추가하지 않는다. 모듈은 caller가 보유한 제어 상태와 OS handler를 받아 새 가변 전역 상태를 만들지 않는다. handler는 종료0/실행1/자식변경을포함한실행3만 남기고, 종료 뒤SIGCHLD가 와도실행으로 되돌리지 않는다. 관리handler의sa_mask에관리시그널전체를넣어중첩갱신을막는다.
첫단위는mask/disposition 설치·복원, epoll_pwait, child-event표시 소비와exec자식mask/default복원이다. 실제waitpid는하지않아부모worker/작업자CGI단일회수책임을유지한다. main/기존blockingCGI는아직연결하지않는다. caller는대기전상태를확인하고EINTR을무조건재시도하지말아야한다.

## 시그널 첫 실행 실패·보강 선택 대기 (2026-11-25)
process_signal_wait.h/c와새단위를구현/컴파일. pending/대기직전6개통과, 항상readable LT I/O에서pending종료우선반례실패. 이후시그널시험미실행, 새단위포함전체make test는통과상태가아니다. main미통합/성능미실행.
원인·원문·두수정안과예상syscall비용은 iteration-5-signal-review.md 참조. wait반환후잠깐unmask/reblock(추천) vs pending조회/0초소비는사용자선택전구현하지않는다. 기존epoll_pwait선택·최소handler·HTTP정책을임의변경하지않음.

## wait 반환 구간 보강 확정 (2026-11-25)
사용자 ‘1반’을 1번 선택으로 확인했다. epoll_pwait 반환 직후 wait_mask를 잠깐 적용해 pending 관리 시그널을 handler에 전달하고 managed_mask를 다시 차단한다. caller는 I/O보다 종료·fatal·자식 상태를 먼저 검사한다. 두 mask 호출이 성공하면 원래 wait 반환값과 errno를 보존한다. mask 변경 실패는 -1로 보고하고, 첫 호출 실패 시에도 재차단을 시도한다. 호출마다 mask 변경 두 번이 추가되며 성능은 측정하지 않는다. 기존 ready I/O 실패 반례는 유지한다.
검증 범위: 기존 시그널 시험 전체, ready 반환값·차단 마스크, EINTR/EBADF 보존, timeout 반환, 기존 단위 회귀. main 통합·실제 CGI·부하 측정은 포함하지 않는다.

## 시그널 보강 검증 완료 (2026-11-25)
1번 구현 후 기존 ready I/O 실패 반례와 시그널·fork/exec·FD 회수 시험 통과. 일반 O2 컴파일 성공, ASan/UBSan 전체 make test 14개 성공(exit 0). 반환값·errno·차단 mask 보존도 확인했다. sigprocmask 실패 강제 주입, main/실제 worker/CGI 통합 E2E는 미실행. 성능 미측정. 상세 흐름·비용·한계는 iteration-5-signal-review.md에 기록했다.
