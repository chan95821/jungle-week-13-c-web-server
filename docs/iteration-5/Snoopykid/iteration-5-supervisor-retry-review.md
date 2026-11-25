# 부모 감독 10ms 제어 재시도 구현 리뷰

2026-11-25. 사용자 1번에 따라 pending 상태일 때만 worker별 임시10ms 재시도를 구현했다. 기존 supervisor 정책·HTTP·backend 선택 정책은 유지했다.

## 실제 함수 흐름

1. `advance_parent_worker_supervisor(now)`가 worker별 상태를 확인한다.
2. `reap_worker()`의 waitpid EINTR 또는 `cleanup_reaped_worker()`의 shared trylock EBUSY에서 `schedule_worker_retry()`가 WAIT/BACKEND 종류와 now+10ms를 저장한다. 숫자는 concurrency_limits.h의 INITIAL_SUPERVISOR_RETRY_MS 한 곳에 있다.
3. 다음 advance가 기한 전에 호출되면 해당 작업만 보류한다. 다른 worker, 종료 신호 전송·기한 확인은 계속 수행한다. 반복 I/O 때문에 같은 trylock을 즉시 반복하지 않는다.
4. `parent_supervisor_wait_timeout_ms()`가 종료 신호 기한과 pending 제어 기한의 최소값을 반환한다. 실제 부모 루프는 이 값을 epoll_pwait 대기 상한에 사용하고, 향후 HTTP/dispatch 기한도 함께 고려해야 한다. 기존 signal_timeout 함수는 신호 전용 조회로 유지한다.
5. 기한에 도달하면 해당 worker당 한 번 재확인한다. 여전히 EBUSY/EINTR면 다음10ms를 예약하고 완료되면 clear_worker_retry로 제거한다. pending이 없고 종료 기한도 없으면 timeout은 -1이다.
6. shared fatal이면 BACKEND 재시도 기한을 기다리지 않고 전체 종료 흐름과 불신 상태 보존을 진행한다. 회수 사실은 여전히 실제 waitpid 반환으로만 확인한다.

이 함수들은 프로젝트 조율 함수다. clock_gettime(CLOCK_MONOTONIC), waitpid, pthread mutex trylock, epoll_pwait는 OS/POSIX/Linux API다. 새 IPC/FD·unlock 통지·가변 전역·처리 경로 할당은 추가하지 않았다.

## 컴파일 및 실제 검증

- 테스트 훅 없는 C11/Wall/Wextra/Werror/O2 모듈 컴파일: 통과.
- make all: 통과.
- ASan/UBSan make test 전체16개 실행 파일: 통과(exit0).
- 실제 LT readable pipe로 반복 wake해도 기한 전 backend trylock 호출수 증가 없음: 통과.
- 다른 worker가 mutex를 푼 마지막 I/O 후 추가요청/알림 없이 epoll timeout으로 회수: 통과.
- 두 사망 worker의 서로 다른 기한에서 due인 worker만 재시도: 통과.
- 실제 자식 종료를 waitid(WNOWAIT)로 확인하고 SIGCHLD를 소비한 뒤 waitpid EINTR 주입, 새 시그널 없이 타이머 회수: 통과.
- 기한 직전1ms/정확히0/지남0, 신호·제어 최소값, pending해제 후 -1: 통과.
- 역행·오버플로우 시간 입력 거부, 기한 전 shared fatal 우선 종료: 통과.
- 기존 격리·2초 강제종료·부분FD·robust 사망복구·다른worker 유지·FD회수: 통과.

실패 이력: 기존 ESRCH 시험은 EINTR 직후 다음 advance의 반환0을 가정했다. 새10ms 계약에 따라 아직 WAIT pending이면 반환1도 정상이다. 해당 기대값을 비음수 허용으로 수정했고, 실제 회수 확인·기한 전 wait 호출수 제한은 별도 시험에서 엄격히 검증했다. 이 수정 전 자동 승인 검토가 사용량 제한으로 완료되지 않아 중단됐고 사용자 재개 후 같은 승인 경로로 실행했다.

최종 로그: build/iteration5-capability/unit-tests-with-supervisor-retry.log. 단독 로그: build/iteration5-capability/supervisor-retry-test.log. 실패 시 시험 자식이 남지 않도록 시험 자식에15초 alarm guard를 추가했다. 이는 운영 worker 수명 정책이 아니다.

## 비용·한계·남은 통합

현재 WSL ABI에서 worker 엔트리는112→120바이트, owner는112바이트 그대로다. W4 총 감독 저장 공간은592바이트로 기존보다32바이트 늘었다. 이는 sizeof 계산이며 서버 전체 메모리 실측이 아니다. pending 동안 재시도 wake/시스템 호출이 생기며 정상 unlock에는 추가 IPC가 없다. CPU/지연/처리량을 측정하지 않았다.

10ms는 예약 간격이며 실제 회수 지연 상한을 보장하지 않는다. OS 스케줄링과 잠금 점유에 영향을 받는다. timeout helper는 관리 기한만 제공하며 제품main·listener/dispatch·worker HTTP 상태 머신은 아직 미통합이다. 제품 서버는 여전히 순차 처리다. 실제 서버 make verify E2E·부하/성능·커밋/머지/푸시를 실행하지 않았다.
