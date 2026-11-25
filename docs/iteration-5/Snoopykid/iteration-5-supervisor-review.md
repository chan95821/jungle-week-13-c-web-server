# Iteration 5 부모 작업자 감독 구현 리뷰

2026-11-25. 사용자 1번 선택을 `parent_worker_supervisor.h/c`에 구현했다. 실제 fork 프로세스로 종료·격리·원장 회수 흐름을 검증했다. 제품 main과 HTTP worker 루프에는 아직 연결하지 않았으며 서버 실행 방식은 기존 순차 서버다.

## main부터 연결할 실제 함수 순서

1. `create_parent_worker_supervisor()`가 부모 전용 worker 표를 고정 할당한다. FIFO·부모 연결 장부·공유 backend view는 빌려 사용한다. 공유 mapping을 다시 만들거나 자체 fork하지 않는다.
2. main이 fork하고 부모/공유 worker 세대를 준비한 후 `adopt_parent_worker()`가 PID·세대·부모 채널 끝을 받는다. 성공 시 caller FD는 -1이 되고 supervisor가 채널과 내장 FD sender를 소유한다. 아직 초기화하지 않은 슬롯이나 다른 worker의 PID를 중복 등록하지 않는다.
3. 모두 준비되면 `finish_parent_worker_startup()`을 호출한다. 부모 dispatch는 `parent_worker_accepts_assignment()`를 먼저 확인한다. 격리된 worker는 기존 장부 세대를 사망 회수 전까지 보존하되 배정에서는 제외한다.
4. CLOSED parser 오류/채널 단절을 감지한 caller가 `request_parent_worker_stop()`으로 해당 worker를 STOPPING으로 옮긴다. 단조 시간 기준 유예는 `INITIAL_WORKER_STOP_GRACE_MS=2000`이며 반복 요청으로 기한을 연장하지 않는다.
5. `advance_parent_worker_supervisor()`는 PID별 `waitpid(WNOHANG)`를 한 번씩 확인하고, STOPPING의 SIGTERM을 한 번 시도한다. 기한에 도달했고 아직 회수되지 않았다면 SIGKILL을 한 번 시도한다. `parent_supervisor_signal_timeout_ms()`가 다음 신호 기한까지 epoll 대기 상한을 제공한다.
6. waitpid가 실제 PID를 반환한 뒤에만 sender abort → 부모 채널 close → 부모 연결 장부 회수 → 공유 trylock → backend 원장 회수를 수행한다. 다른 worker의 세대·카운터는 유지한다. 채널을 닫으면 실제 epoll 등록은 OS가 해제하지만, caller가 보관한 stale event도 worker 상태로 걸러야 한다.
7. shared mutex EBUSY는 기다리지 않고 cleanup_pending/반환1로 남긴다. 해당 worker는 REAPED지만 FINISHED가 아니며 재사용하지 않는다. 다른 worker의 제어/I/O는 계속 가능하다. 완료 재시도 시 부모 장부 회수가 중복 감소하지 않는다.
8. SIGINT/SIGTERM을 관측한 main은 `request_parent_supervisor_shutdown()`을 먼저 호출해야 한다. shared fatal은 advance에서 직접 확인한다. 전체 종료는 accept/배정 gate를 닫고 FIFO FD와 queued 장부를 정리하며 모든 worker에 동일 유예를 적용한다. 남은 유효 worker가 0이면 오류 종료를 요청한다. 자동 respawn/재전달은 없다.
9. `parent_supervisor_all_reaped()`와 `parent_supervisor_cleanup_finished()`를 구분한다. 후자 확인 뒤 `destroy_parent_worker_supervisor()`가 부모 표만 해제한다. caller가 별도로 공유 mutex/mapping 등 빌려준 소유자들을 정리한다. 불명확한 PID·미회수/미완료 worker가 있으면 supervisor destructor는 EBUSY다.

POSIX 함수는 fork/kill/waitpid/close/clock_gettime이며 위 조율 함수들은 프로젝트에서 직접 작성했다. SIGKILL 성공이 즉시 회수를 보장하지 않으므로 2초를 전체 종료 완료 기한이라고 하지 않는다.

## 불변 조건·오류 처리

- 각 PID의 유일한 waitpid 소유자는 부모 supervisor다. SIGCHLD를 SIG_IGN/SA_NOCLDWAIT로 설치하면 안 된다. CGI 자식 PID는 이 표에 등록하지 않는다.
- waitpid EINTR는 다음 진행으로 미루고 회수 사실을 만들지 않는다. kill ESRCH도 사망 확인으로 취급하지 않는다.
- wait 소유권 유실/ECHILD는 제한된 첫 오류를 기록하고 전체 종료를 요청한다. 해당 PID를 LOST로 유지해 다시 신호를 보내거나 회수 완료라고 하지 않는다. PID 재사용·신뢰할 수 없는 수명에 대한 보수적 방어다. 자동 복구나 강제 destructor 우회는 없다.
- shared 손상·복구 불가면 전체 종료한다. 손상된 원장을 reset해 정상처럼 만들지 않고 backend 회수 실패를 표시한다. 실제 자식이 모두 종료한 뒤 공유 영역을 버릴 수 있지만, 정상 회수가 끝났다고 표현하지 않는다.
- 진단은 worker index/generation, 실패 연산 enum/errno, 기존 backend 검사·복구 단계 숫자를 고정 구조에 첫 오류로 보존한다. 요청/본문/헤더/가변 문자열은 기록하지 않는다. 실제 log sink 출력은 미통합이다.
- 장부·네트워크 close 및 kill/waitpid는 공유 mutex 밖에서 실행한다. 공유 lock 안에는 기존 bounded 원장 검증·회수만 있다. 가변 전역 상태·추가 thread·재시작 관리기를 만들지 않았다.

## 컴파일과 실행 결과

| 구분 | 결과 |
| --- | --- |
| 테스트 훅 없는 일반 C11/Wall/Wextra/Werror/O2 모듈 컴파일 | 통과 |
| `make all` (기존 제품 서버 빌드) | 통과 |
| ASan/UBSan `make test`, 16개 실행 파일 | 전부 통과, exit0 |
| 실제 잘못된 CLOSED → 해당 worker 격리/TERM/waitpid, 다른 worker socket I/O 및 두 장부 유지 | 통과 |
| 실제 SIGTERM 무시 자식, 2초 유예·SIGKILL 1회·waitpid 후 해제 | 통과 |
| 실제 부분 FD sender/SCM_RIGHTS 앵커 후 자식 SIGKILL, 원본·채널·두 원장 정리 | 통과 |
| 다른 worker 잠금 보유 중 EBUSY defer, 살아 있는 worker 명령/응답 후 재회수 | 통과 |
| 실제 epoll_pwait와 SIGTERM/SIGCHLD, FIFO FD/queued 수 정리 | 통과 |
| shared fatal·일관성 모순 → 제한 숫자 진단·전체 worker 회수·불신 원장 보존 | 통과 |
| 실제 robust mutex 보유자 SIGKILL → owner-death 복구/나머지 서비스 | 통과 |
| wait EINTR·kill ESRCH/EPERM 주입, 실제 wait 소유권 유실 ECHILD의 신호/해제 차단 | 통과 |
| 마지막 worker 소실 → accept 불가·전체 오류 종료, 반복 호출 및 FD 개수 복원 | 통과 |

최종 로그는 `build/iteration5-capability/unit-tests-with-supervisor.log`다. 최초 테스트 컴파일의 misleading-indentation 경고를 수정한 후 위 실행을 확인했다. 실제 fork fixture는 제품 HTTP 서버 E2E가 아니다. 자식들은 테스트용 pause/echo/poll 작업이며 완성된 HTTP 이벤트 루프로 표현하지 않는다.

ECHILD 시험은 일부러 부모 밖에서 실제 wait 결과를 소비해 계약 위반을 재현한다. 시험 종료 정리는 시험이 직접 보유한 실제 회수 증거로 수행하며, 제품 코드에서 LOST를 강제로 완료로 바꾸는 우회 API는 없다.

## 저장 공간·정상 비용

현재 WSL ABI에서 supervisor owner=112바이트(진단56 포함), worker 엔트리=112바이트(sender 포함)다. W4 기준 고정 추가 할당은 112+4×112=560바이트다. 기존 채널을 인수하며 새 FD·shared mapping·가변 요청 버퍼를 만들지 않는다. 생성 후 확장 할당은 없다.

advance 호출마다 최대 W개의 WNOHANG waitpid, 사망 worker마다 최대 한 번 trylock을 수행한다. SIGTERM/SIGKILL은 각각 worker당 한 번 시도한다. 실제 CPU/메모리 전체/처리량/지연을 측정한 값이 아니다.

## 남은 통합과 명시적 한계

- main의 listener/worker/fatal pipe LT 등록과 실제 dispatch/worker HTTP 상태 머신은 미통합이다. 기존 blocking handle_client를 이벤트 callback에 넣지 않았다.
- shared mutex EBUSY 후 재시도를 깨울 정책·간격은 아직 선택하지 않았다. deadline helper는 종료 신호 기한만 제공한다. cleanup_pending을 무시하고 무기한 epoll 대기하면 회수가 지연될 수 있으며, 반대로 무조건 0초 대기는 busy loop가 될 수 있다. 실제 부모 루프 연결 전에 합의해야 한다.
- log sink 적체/종료 처리, 부모 사망 후 자식 수명, 네 선택 정책 시간·점수·입력, CGI/파일 I/O 전환은 남아 있다.
- kill 권한 오류나 중단 불가 커널 대기에서 강제 회수 완료를 보장하지 않는다. 확인되지 않은 자식이 있는 동안 공유 영역을 명시적으로 해제하지 않는다.
- 테스트 driver의 20ms 진행 간격과 5초/45초 hang guard는 테스트 전용이며 운영 재시도 정책이나 성능 지표가 아니다.
- 제품 `make verify` E2E·부하·성능 측정·커밋·머지·푸시는 실행하지 않았다.
