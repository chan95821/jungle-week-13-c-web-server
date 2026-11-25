# Iteration 5 시그널 대기 보강 결과

2026-11-25. 사용자 1번 선택에 따라 wait 반환 후 handler 전달 구간을 구현했다. 앞서 실패했던 ‘계속 readable인 LT I/O와 pending 종료’ 반례를 유지했고 수정 후 통과했다. 최초 실패 기록과 원인 분석은 Git 변경 이력 및 설계 문서의 당시 기록으로 구분한다.

## 실제 함수 순서

1. `prepare_process_signal_wait()`는 caller 소유 플래그를 빌리고 INT/TERM/CHLD를 차단한다. 원래 mask/action을 저장하고 최소 handler와 SIGPIPE ignore를 설치한다.
2. OS handler가 호출하는 `record_process_signal()`은 sig_atomic_t 플래그만 변경한다. 종료 후 SIGCHLD가 실행 상태를 되살리지 않는다.
3. `wait_process_signal_events()`는 `epoll_pwait()`로 대기하고 반환값과 errno를 저장한다. 반환 직후 wait_mask로 pending handler 전달을 허용한 다음 managed_mask를 재차단한다. 두 mask 호출 성공 시 원래 결과·errno를 돌려준다. 첫 mask 호출 실패 시에도 재차단을 시도하고 실패는 -1로 보고한다. caller는 오류 또는 종료/fatal 상태를 I/O 처리보다 먼저 확인해야 한다.
4. `take_process_child_signal()`은 표시만 소비한다. 실제 waitpid와 작업자 세대·기여분 회수는 부모가 담당한다. SIGCHLD 병합을 고려해 종료 시에도 자식 PID를 확인해야 한다.
5. `restore_process_signal_wait()`는 수명 종료 시 원래 action/mask를 복원한다. `restore_exec_child_signals()`는 fork/exec 자식에서 관리 disposition을 기본값으로, mask를 서버 시작 전 상태로 복원한다.

`main()`과 실제 부모·작업자 이벤트 루프에는 아직 연결하지 않았다. 현재 제품 서버는 순차 처리 상태다. 실제 CGI 회수 책임·I/O 전환도 남아 있다.

## 검증 결과

| 구분 | 결과 |
| --- | --- |
| 일반 C11/Wall/Wextra/Werror/O2 컴파일, 테스트 훅 없음 | 통과 |
| ASan/UBSan 전체 `make test`, 14개 실행 파일 | 모두 통과, exit 0 |
| 차단 중 pending 및 wait 직전 INT/TERM/CHLD 6개 | 통과 |
| 계속 readable인 LT I/O와 pending SIGCHLD·SIGTERM | 통과, 기존 실패 반례 유지 |
| ready/timeout/EBADF 반환, EINTR, errno 보존, 반환 시 차단 mask | 통과 |
| 실제 자식의 비동기 종료 신호·여러 자식 caller 회수 | 통과 |
| pipe 알림 전 자식 사망, READY/WRITING 긴급 보고와 SIGCHLD | 통과 |
| fork/exec disposition·mask 복원, 무관한 SIGUSR1 mask 보존 | 통과 |
| 정상 종료 FD 개수 복원 | 통과 |

통과 로그: `build/iteration5-capability/unit-tests-with-signal-wait.log`.
최초 실패 로그는 `build/iteration5-capability/process-signal-test.log`에서 단독 재실행 시 통과 로그로 갱신됐다. 최초 exit134 및 원인은 당시 문서 기록에 남아 있다.

## 비용·미검증 범위

현재 WSL ABI에서 context는 1008바이트이며 이 모듈이 소유하는 FD·heap은 없다. wait 반환마다 mask 변경 syscall 두 번이 추가된다. 이는 코드상의 호출 수이고 CPU·처리량·지연 측정 결과가 아니다.

유효한 고정 인자를 쓰는 sigprocmask의 실패 분기는 구현했으나 강제 오류 주입은 하지 않았다. 무관한 외부 handler의 errno 변경, 임의 메모리 손상 복구를 보장하지 않는다. 향후 루프는 mask 오류 시 정상 I/O를 이어가면 안 된다.

실제 서버 E2E `make verify`, 부모·작업자 main 통합, 실제 CGI, 부하·성능 측정은 이번 실행 범위에 포함하지 않았다. 커밋·머지·푸시하지 않았다.

## 원인 근거

Linux 6.18 `do_epoll_pwait()`는 EINTR이 아닌 반환 경로에서 저장 mask를 복원한다. 계속 ready인 I/O가 먼저 반환되면 pending 종료가 handler에 전달되지 않을 수 있었다. 대기 진입의 원자성만으로 종료 우선 처리를 가정하지 않고 반환 후 전달 구간을 둔다.

- https://raw.githubusercontent.com/torvalds/linux/v6.18/fs/eventpoll.c
- https://man7.org/linux/man-pages/man2/epoll_wait.2.html
