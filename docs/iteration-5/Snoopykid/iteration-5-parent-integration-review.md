# 부모 HTTP 루프 통합 리뷰 — 2026-11-25

사용자 지시에 따라 이번 확인은 컴파일·링크와 통합 흐름의 최소 실행으로 제한했다. 전체 회귀와 광범위한 장애 주입, 성능 측정은 실행하지 않았다.

## 구현과 실제 호출 순서

제품 src/main.c는 아직 기존 순차 실행을 유지한다. 아래 호출은 tests/unit/test_process_http_smoke.c의 실행 가능한 통합 진입점에서 조립한다.

1. 테스트 main은 기존 C 백엔드 3개와 외부 TCP 리스너 하나를 준비한다.
2. 부모는 공유 상태·연결 장부·FIFO·감독 상태를 만들고 create_parent_http_loop()로 LT epoll을 준비한다. fork()로 고정 작업자 4개를 만들고 각 IPC 채널을 감독 상태에 등록한다.
3. run_parent_http_loop() → accept_pending_clients(): accepted FD와 접수 시각을 부모 장부/FIFO에 보관한다. Q/G 한도에 닿으면 accept 관심을 보류한다.
4. dispatch_queued_clients()는 설정된 worker 정책과 사용 가능한 용량으로 작업자를 고른다. advance_sender()가 기존 SCM_RIGHTS 부분 전달을 진행한다. 완전 인계 후 부모 원본 FD를 닫으며, 전송된 FD를 다른 작업자에게 재배정하지 않는다.
5. 작업자는 advance_worker_http_reactor()에서 요청 파싱·본문 수신·백엔드 선택/예약·non-blocking connect/송수신·클라이언트 응답을 진행한다. 공유 잠금 안에서 네트워크 I/O를 수행하지 않는다.
6. 메모리 경로는 request_ready() → prepare_known_memory_route() → 기존 prepare_hello/prepare_echo/prepare_greet() → resume_worker_http_memory_response() 순이다. 기존 본문/라우팅 정책을 재사용한다. 생성된 greet 메모리는 연결 슬롯이 소유하며 종료 정리에서 해제한다. 파일/CGI provider는 여기서 호출하지 않는다.
7. read_worker_completions()가 CLOSED를 읽어 부모 장부와 분배 통계를 갱신한다. FD 인계 종료보다 빠른 CLOSED는 기존 early-completion 규칙대로 처리한다.
8. SIGTERM/SIGINT 또는 긴급 실패 통지는 감독 경로로 연결된다. 작업자 회수와 공유 예약 회수의 선후 관계를 유지한다. 종료 중 epoll 실패 시에만 신호 대기용 pselect로 종료 처리를 이어간다.

## 소유권

부모 루프는 자신의 epoll/관심 상태 배열을 소유하고 리스너·긴급 통지 FD·감독 상태를 빌린다. 접수 FD는 FIFO → 부모 sender → 작업자 순으로 이동한다. 장부는 FD를 소유하지 않는다. 작업자는 클라이언트/백엔드 FD와 연결 버퍼를 소유한다. 공유 mapping과 mutex는 작업자 전원 회수 증명 후 파괴한다. waitpid 소유권 상실 등 회수 불명 상태에서는 공유 소유자를 파괴하지 않는다.

## 이번 실행 결과

- 컴파일·링크: make all worker-io build/test_process_http_smoke 성공. strict C11, -Wall -Wextra -Werror, 일반 -O2 구성이다.
- 최초 컴파일에서 테스트의 O_CLOEXEC 선언에 필요한 fcntl.h 누락을 발견해 수정했다. 수정 후 컴파일과 아래 최소 실행이 통과했다.
- 최소 통합: 기존 C 백엔드 + 실제 TCP 리스너 1개 + 부모 + 작업자 4개. 느린 조회 A가 미완료인 동안 조회 B·hello·바이너리 제출·greet가 정상 완료됐다. greet는 느린 조회와 같은 작업자에 배정된다. 지연 수치를 수집한 성능 측정이 아니라 완료 순서와 응답 내용을 확인한 기능 검사다.
- 종료: 백엔드 대기 중 부모 SIGTERM → 작업자 4개 회수, 남은 클라이언트 정리, FD 기준선 복귀, 부모 연결/대기 큐/백엔드 예약 0 확인.
- 증거: build/iteration5-capability/process-http-smoke.log의 PASS 2개. 이 실행 파일은 ASan/UBSan 구성이 아니다.
- 미실행: 이번 변경 이후 전체 make test/make verify, 광범위한 장애 주입, 부하·성능 실험. 이전 20개 sanitizer 통과 결과는 이번 변경 이후 결과로 간주하지 않는다.

## 남은 제품 통합 경계

정적 파일과 CGI의 이벤트 루프 대응이 필요하다. 정적 파일의 O_NONBLOCK 또는 sendfile/mmap만으로 디스크 대기·페이지 폴트가 작업자를 막지 않는다고 보장할 수 없다. CGI는 비동기 파이프·자식 회수·취소 수명을 연결해야 한다. 이 선택 전 기존 제품 경로를 비활성화하거나 blocking handle_client를 reactor에서 호출하지 않는다.

파일 I/O 대안(미채택):
- 작업자별 제한된 보조 스레드: 파일 작업 위임과 완료 통지, 상한 있는 큐가 필요하다. 프로세스/FD 전달은 줄지만 스레드 동기화·종료와 다중 스레드 상태의 CGI fork/exec 안전성을 함께 설계해야 한다.
- 제한된 보조 프로세스: 파일 대기를 네트워크 작업자 밖에 격리한다. 추가 프로세스·IPC·버퍼 소유권·사망 회수 비용이 있다. 개수와 전송 규약은 미결정이다.
- 작업자에서 제한된 직접 파일 읽기: 구현량은 작지만 디스크 대기가 다른 연결을 막을 수 있다. 비차단 요구의 예외를 사용자가 선택해야 하며 자동 채택하지 않는다.

제품 main 전환과 최종 회귀 체크포인트는 이 경계를 합의한 뒤 진행한다. 최종 분배 알고리즘과 성능 실행 허락은 별도다.
