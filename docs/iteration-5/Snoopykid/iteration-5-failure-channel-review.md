# Iteration 5 긴급 pipe와 고정 진단: 코드 리뷰·검증 결과

2026-11-25. 별도 non-blocking pipe와 작업자별 고정 진단칸을 구현했다. **진단 게시·깨우기 단위가 완료된 것이며, 실제 서버의 전체 종료 절차는 아직 연결하지 않았다.**

## 변경 파일과 적용 범위

- `src/worker_failure_channel.h/c`: pipe 생성, 알림·소비, 진단칸 세대 관리·게시·조회·고정 형식 출력.
- `src/shared_backend_state.h/c`: 공유 레이아웃 version 3. 기존 익명 매핑에 작업자당 진단칸 하나를 추가하고 atomic 지원을 검사한다.
- `tests/unit/test_worker_failure_channel.c`, `Makefile`: 독립 단위 테스트를 추가했다.

클라이언트 FD 전달은 기존 STREAM/no-ACK를 유지한다. 일반 CLOSED 제어 메시지, `main()` 통합, CLI, 부모·작업자 이벤트 루프는 이번에 변경하지 않았다. 실제 정적 루트·권한·`proxy.conf`도 유지했다.

## main부터 연결할 순서

다음은 후속 통합의 호출 순서다. 현재 `main()`이 이미 이 코드를 실행하는 것은 아니다.

```text
main()
 → create_shared_backend_memory()
 → create_worker_failure_pipe()
 → arm_worker_failure_slot()      초기 작업자들은 모두 fork 전에 준비
 → fork()
    ├─ 작업자: backend 잠금 해제 → publish_worker_failure()
    │                            → notify_parent_worker_failure()
    └─ 부모: LT epoll 알림 → fatal 확인·새 accept 중단
                         → consume_worker_failure_wakeup()
                         → read_worker_failure_report()
                         → 고정 형식 로그 준비 → 전체 종료 절차
```

부모가 알림을 받기 전에 작업자가 죽는 경로도 있으므로, 실제 통합에서는 SIGCHLD 처리에서도 fatal과 진단칸을 확인해야 한다.

## 실제 함수의 책임

| 함수 | 동작·계약 |
| --- | --- |
| `create_worker_failure_pipe()` | POSIX `pipe()`와 `fcntl()`로 양끝을 NONBLOCK/CLOEXEC로 설정한다. 실패하면 두 FD를 정리한다. fork 후 부모는 읽기 끝, 작업자는 쓰기 끝만 남긴다. |
| `arm_worker_failure_slot()` | 부모가 초기 fork 전 또는 `waitpid()`로 이전 작성자의 종료를 확인한 뒤 호출한다. 세대는 증가해야 하며, 전역 fatal 이후 재설정은 거절해 증거를 보존한다. 작업자 자동 재시작 함수가 아니다. |
| `publish_worker_failure()` | backend 잠금을 보유하면 EPERM. 현재 세대를 확인하고 fatal 의도를 먼저 남긴 뒤 EMPTY→WRITING→READY로 게시한다. 같은 세대의 첫 보고를 덮어쓰지 않는다. |
| `notify_parent_worker_failure()` | 한 번의 non-blocking `write()`로 알림 1바이트를 보낸다. 성공, 이미 읽을 데이터가 있음(EAGAIN), 재시도 필요(EINTR), 오류(EPIPE 등)를 구분한다. 쓰기 실패로 진단이나 fatal을 지우지 않는다. |
| `consume_worker_failure_wakeup()` | 한 번에 최대 64바이트만 읽는다. 알림·빈 상태·EOF·EINTR·오류를 구분한다. 무한 drain이나 busy-spin을 하지 않는다. |
| `read_worker_failure_report()` | 부모가 아는 작업자·세대와 대조한다. READY를 acquire로 확인한 뒤에만 내용을 복사한다. EMPTY·WRITING·과거 세대에서는 출력 객체를 바꾸지 않는다. 신뢰할 수 없는 보고는 fatal/EUCLEAN으로 처리한다. |
| `format_worker_failure_report()` | reporter와 영향받은 작업자·세대, 실패한 검사, phase·stage, 종료 이유를 숫자로만 작성한다. reason 1은 복구 불가 공유 상태다. |
| `format_missing_worker_failure_report()` | 부모가 알고 있는 작업자·세대만 기록하고, 알 수 없는 대상·검사·단계는 `unknown`으로 명시한다. 미게시·미완료·과거 세대·손상 상태를 구분한다. |

두 formatter는 실제 로그 sink에 쓰지 않는다. 공간이 부족하면 ENOSPC를 반환한다. 이유 문자열은 코드에 고정된 문자열만 사용하며, 요청 본문·URI·헤더·공유 문자열·포인터를 출력하지 않는다.

진단칸은 세대당 작성자 하나, 게시 후 불변이라는 계약을 따른다. 부모는 조회와 재설정을 동시에 수행하지 않는다. 정상 요청마다 긴급 pipe syscall을 수행하지 않는다. 제품 코드에 새 가변 전역 상태나 테스트 callback은 없다.

## 게시 도중 사망

| 사망 지점 | 남을 수 있는 상태 | 부모가 해석할 내용 |
| --- | --- | --- |
| fatal 의도 표시 직후 | fatal=1, 진단칸 EMPTY | 진단이 없어도 안전 상태가 아니다. 원인은 추정하지 않는다. |
| claim·작성 도중 | WRITING | 내용을 읽지 않고 미완료·unknown으로 표시한다. |
| READY 후 pipe 쓰기 전 | 완성된 진단, pipe 알림 없음 | 사망 후에도 진단은 보존된다. SIGCHLD 경로가 필요하다. |
| pipe 쓰기 후 | READY와 깨우기 알림 | 읽기 가능한 진단을 수집한다. |

따라서 pipe만 감시하며 무한 대기하는 구현은 충분하지 않다. **SIGCHLD와 epoll 대기의 경합 방지, 부모 사망 처리, 로그 적체와 강제 종료 기한은 아직 미통합이다.** EINTR 결과를 받은 호출자의 재시도·종료 연계도 남아 있다.

부모는 pipe 배출 전후에 fatal을 확인해야 한다. pipe가 찬 EAGAIN은 이미 읽을 데이터가 있다는 뜻이므로, 추가 쓰기를 기다리지 않는다. pipe 바이트 자체는 진단이나 종료 명령의 원문이 아니라 깨우기 신호다.

robust mutex와 이 진단칸은 임의 메모리 손상을 복구하거나 감사를 위한 무결성을 보장하지 않는다. 일부 상태·인덱스·세대·이유의 모순은 검사하지만, 그럴듯하게 일치하는 손상이나 fatal 자체의 손상까지 탐지한다는 보장은 없다.

## 메모리·FD 비용

현재 WSL ABI에서 계산한 크기다. 전체 서버 RSS나 처리 능력의 측정값이 아니다.

| 항목 | 크기 |
| --- | ---: |
| 보고 내용 | 48바이트 |
| atomic 세대·게시 상태를 포함한 작업자별 진단칸 | 64바이트 |
| 작업자 4개 추가분 | 256바이트 |
| W=4, R=1,024의 전체 공유 매핑 | 33,416바이트, 약 32.6KiB |
| 선택한 공유 메모리 예산 | 1MiB |
| 부모의 로그 준비 버퍼 | 호출당 384바이트 |

기존 header 232, worker 40, reservation 32, undo record 128바이트는 유지했다. 검증 scratch도 프로세스당 96바이트로 같다. 매핑은 예산 1MiB 전체를 선할당하지 않고 필요한 길이만 사용한다. 진단칸의 atomic이 lock-free가 아니면 생성 시 ENOTSUP로 실패한다.

pipe는 생성 시 FD 두 개다. fork 후 불필요한 사본을 닫아 부모는 읽기 참조 하나, 각 작업자는 쓰기 참조 하나를 유지해야 한다. 커널 pipe 용량을 고정값으로 가정하거나 OS 설정을 변경하지 않았다. SIGPIPE 무시는 기존 서버 정책을 전제로 하며, 이 모듈이 전역 시그널 정책을 바꾸지는 않는다.

## 컴파일과 실제 실행

**컴파일:** hook 없는 C11·Wall·Wextra·Werror·O2·pthread 컴파일 성공. 새 단위 테스트의 ASan/UBSan 빌드도 성공했다.

**실제 실행:** 최종 `make test`의 13개 바이너리 모두 통과했다. 실패·skip은 없고 sanitizer 진단도 없었다.

| 검증 | 결과 |
| --- | --- |
| NONBLOCK/CLOEXEC, 빈 상태·알림·EOF, 실제 LT epoll 깨우기 | 통과 |
| 실제 pipe 포화, EAGAIN 후 상태 보존, 부분 배출 후 LT 재알림 | 통과 |
| 일반 STREAM 송신 적체를 유지한 채 별도 pipe 깨우기·조회 | 통과 |
| EINTR 분기 주입, 실제 EPIPE, 진단 보존 | 통과 |
| 게시·깨우기 5개 경계의 실제 fork/SIGKILL | 통과 |
| barrier로 출발한 4개 작업자의 각자 보고 | 통과 |
| 첫 보고 유지, 이전 세대 차단, 정상 종료 확인 후 세대 준비 | 통과 |
| 진단 상태·내용 손상 5종의 거절과 unknown 출력 | 통과 |
| backend 불변 조건 실패 후 잠금 밖 보고 | 통과 |
| 실제 ENOTRECOVERABLE mutex 상태에서도 진단 게시·조회 | 통과 |
| pipe·소켓·epoll·매핑·자식 정리와 FD 개수 복귀 | 통과 |

사망 경계 테스트는 다른 작업자의 열린 writer를 모델링해, 알림 전 사망 시 pipe가 조용할 수 있음도 확인했다. 테스트에서는 `waitpid()`로 직접 사망을 확인했으며 실제 부모 SIGCHLD 이벤트 루프를 검증한 것은 아니다. EINTR은 wrapper 분기 주입이다. 테스트의 alarm·harness·poll 제한은 운영 기한으로 채택하지 않는다.

최종 로그는 `build/iteration5-capability/unit-tests-with-failure-channel.log`다. 첫 신규 단위 실행은 `worker-failure-test.log`에 있으며, 이후 추가한 unknown formatter 검증까지 포함한 결과는 전체 unit 로그가 최신이다.

## 남은 범위

- `main()`·부모/작업자 epoll, CLOSED 제어 메시지, 전체 연결 장부, 작업자 생성·회수.
- SIGCHLD와 대기 경합, 부모 사망, 실제 로그 sink와 전체 종료, 강제 종료 기한·재시작 정책.
- pipe/fcntl 초기화 실패 주입, 비 lock-free 플랫폼, 다른 OS, 로그 sink 적체, 실제 TCP worker 전체 종료 E2E.

서버 `make verify`, 부하·성능 측정은 실행하지 않았다. 커밋·머지·푸시도 하지 않았다.
