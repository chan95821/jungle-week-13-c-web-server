# 파일 보조 스레드 통합 리뷰 — 2026-11-25

## 채택과 구현 경계

사용자 1번으로 작업자별 제한된 보조 스레드 방식을 채택했다. 현재 시작 구성은 네트워크 작업자 프로세스마다 파일 전용 보조 스레드 하나다. 최종 성능 튜닝값이라는 뜻은 아니다. 큐 용량은 기존 작업자 연결 상한 C에서 유도하고, 연결 슬롯마다 파일 job 하나·동시에 한 작업만 허용한다. 별도의 무제한 작업 큐나 파일 전체 크기 버퍼는 만들지 않았다.

기존 정적 경로(/cafe, /menu, /assets/)의 경로·MIME·오류 정책은 재사용했다. 실제 디스크 I/O는 기존 handler/provider가 수행하지만 호출 위치를 보조 스레드로 옮겼다. 파일 FD에는 O_CLOEXEC를 추가하여 향후 exec에 상속되지 않게 했다. CGI handler는 이 보조 스레드에서 호출하지 않는다. 제품 main의 순차 경로는 아직 유지한다.

## 실제 함수 흐름

1. 통합 실행 진입점 → fork() → create_worker_http_reactor() → create_worker_file_pool(). 부모가 스레드를 가진 상태로 worker를 fork하지 않으며, 각 자식에서 보조 스레드를 만든다.
2. 요청 파싱/본문 수신 후 request_ready()가 기존 라우터 결과를 확인한다. 메모리 handler는 기존 흐름을 유지하고, 파일 handler는 worker_file_start_response()로 전달한다.
3. start_worker_file_job()은 요청 path만 별도로 복사한다. helper가 클라이언트 수신 버퍼나 해제된 요청 본문을 참조하지 않는다. queue_worker_file_operation(PREPARE)으로 제한 큐에 넣는다.
4. run_file_helper()는 로컬 mutex 아래 큐에서 하나를 꺼낸 뒤 잠금을 풀고 prepare_cafe/menu/assets()를 실행한다. realpath/open/fstat 및 provider read/cleanup은 잠금 밖에서만 한다. 공유 backend 상태나 소켓에는 접근하지 않는다.
5. 완료 상태를 mutex로 게시하고 non-blocking/CLOEXEC pipe에 힌트를 보낸다. 이벤트 루프는 WORKER_FILE_EVENT → drain_worker_file_notifications() → worker_file_service_completions()로 완료를 받는다. pipe는 깨우기 힌트이고 job의 완료 상태가 기준이다.
6. PREPARE 완료 시 worker_http_prepare_file_headers()로 응답 헤더를 준비한다. READ는 보조 스레드에서 기존 provider.next_func()를 호출한다. 이벤트 루프는 반환된 16KiB 이하 조각을 빌려 기존 부분 send 경로로 송신한다.
7. 조각이 모두 송신되기 전 다음 READ를 제출하지 않는다. 최초 길이보다 많은 데이터는 제외하고, 해당 길이가 끝나면 추가 read 없이 CLEANUP을 요청한다. 조기 EOF/read 오류는 실패이며, 최종 헤더 송신 후에는 새 HTTP 오류를 붙이지 않는다.
8. CLEANUP은 보조 스레드에서 provider FD/상태를 정리한다. release_worker_file_job()까지 완료한 뒤 정상 drain/close를 진행한다. 빈 파일도 헤더가 송신되기 전에 shutdown하지 않는다.
9. 클라이언트 오류/취소 중 helper 작업이 남아 있으면 슬롯과 job을 유지한다. 현재 작업 완료 후 CLEANUP하고, 그 전에는 CLOSED 통지나 슬롯 재사용을 허용하지 않는다. 연결 세대가 바뀐 슬롯을 늦은 파일 완료가 수정하지 않는다.

## 스레드·시그널·종료

파일 helper는 SIGINT/SIGTERM/SIGCHLD를 차단한 마스크로 생성한다. 네트워크/제어 스레드만 관리 시그널을 받는다. 일반 마스크 변경은 pthread_sigmask로 바꾸었고 fork/exec 자식 복원은 async-signal-safe sigprocmask를 유지했다.

destroy_worker_file_pool()은 새 작업을 막고 helper를 깨워 현재 디스크 호출 뒤 모든 provider를 정리하고 join한다. 디스크 syscall을 pthread_cancel로 강제 취소하지 않는다. 따라서 파일 syscall이 멈춘 경우 join도 기다릴 수 있다. 기존 부모 감독자는 임시 2초 후 SIGKILL을 보내지만, 커널의 uninterruptible I/O에서는 그 신호도 즉시 회수를 보장하지 않는다. waitpid로 종료를 확인하기 전 공유 소유자를 파괴하지 않는 원칙을 유지한다.

CGI는 아직 미통합이다. 여러 스레드가 있는 프로세스에서 기존 prepare_cgi의 fork 뒤 일반 라이브러리 호출을 그대로 실행하지 않는다. 비동기 파이프/자식 회수와 안전한 exec 시작 경로를 연결한 뒤 제품 main을 전환해야 한다.

## 자원 상한과 미측정 비용

- 보조 스레드/완료 pipe: 작업자당 1개/1쌍. 별도 파일당 스레드는 없다.
- pool 포인터 배열: job 위치와 큐 각각 C개. 64bit C256이면 합계 4096bytes이며 소규모 mutex/cond/상태가 추가된다.
- job/provider/path는 파일 요청에서만 할당한다. path는 기존 요청 헤더 8KiB 이내, 기존 file provider 버퍼는 16KiB다. C256 전체가 파일 요청이면 두 저장 공간 합계의 거친 상한은 작업자당 약 6MiB이며 job 메타데이터·allocator·스레드 스택·연결 버퍼·커널 캐시는 별도다. 실측 RSS가 아니다.
- 기본 pthread 스택은 시스템 설정에 따른다. 스택/큐 크기의 최적화나 CPU/RSS 성능 측정은 하지 않았다.
- helper 하나가 느린 파일에 묶이면 같은 작업자의 다른 파일 작업은 큐에서 기다린다. 네트워크/메모리/프록시 I/O는 계속 처리한다. 파일 작업 전용 새 전체 기한이나 자동 재시도 정책을 추가하지 않았다.

## 컴파일과 최소 실행 결과

- strict C11/Wall/Wextra/Werror/O2: make all worker-io build/test_worker_file_io build/test_process_http_smoke 성공.
- 초기 worker_file_bridge.c 기능 매크로 누락으로 struct sigaction 선언 컴파일 실패를 발견하고 _GNU_SOURCE를 추가해 수정했다.
- make smoke-file-io: 테스트 전용 느린 handler가 멈춘 동안 호출자 진행·두 칸 큐·중복 작업 거부·정리·FD 기준선 통과.
- make smoke-process-http: 실제 부모/작업자 4개/C 백엔드. 임시 fixture의 128KiB 바이너리 여러 조각 일치·빈 파일·404·cafe, 느린 조회 중 빠른 조회/제출/로컬 요청 완료, SIGTERM 후 작업자/FD/장부/예약 정리 통과. 실제 public 내용/권한은 변경하지 않았다.
- 직접 변경한 시그널 대상 test_process_signal_wait만 ASan/UBSan으로 실행해 readiness/시그널 우선순위/마스크/exec/FD 정리 통과.
- 실행 로그: build/iteration5-capability/file-helper-minimal.log, file-helper-signal-check.log.
- 미실행: 전체 make test/make verify, 광범위한 파일/worker 장애 주입, 종료 중 실제 멈춘 디스크, 데이터 경쟁 검사(TSan), 부하·성능 측정. 기능 최소 검사가 이런 항목의 보증을 대신하지 않는다.

## 다음 체크포인트

CGI 비동기 실행과 종료·회수 경계를 연결한 뒤 제품 main에 현재 부모/작업자 조립을 옮긴다. 실행 가능한 전체 흐름 체크포인트에서 회귀와 광범위한 장애 주입 범위를 모아 검토한다. 이번 선택은 최종 분배 알고리즘이나 성능 실행 허락이 아니다.
