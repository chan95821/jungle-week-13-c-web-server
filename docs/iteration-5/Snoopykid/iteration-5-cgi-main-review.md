# CGI 비동기 처리·제품 main 통합 리뷰 — 2026-11-25

## 결과와 구현 흐름

제품 ./build/server <port> <proxy.conf>를 고정 프로세스 작업자 4개와 작업자별 LT epoll 흐름에 연결했다. 기존 필수 CLI·HTTP/라우팅·분배 정책 설정을 유지한다. 파일·메모리·프록시·CGI의 실행 가능한 통합 경로가 만들어졌으며, 전체 회귀/광범위 장애 검증을 마쳤다는 뜻은 아니다.

1. main()은 인자와 설정/public 디렉터리를 검사하고 prepare_process_signal_wait(), open_listenfd()를 호출한다.
2. run_process_http_server() → prepare_runtime(): fork 전에 익명 공유 mapping/robust mutex·부모 연결 장부·FIFO·감독 상태·긴급 pipe·부모 epoll을 준비한다.
3. start_network_workers()는 공유 세대를 활성화하고 고정 자식 4개를 만든다. 각 자식은 부모 listener/epoll/다른 IPC FD를 닫고 run_network_worker()로 진입한다. 부모는 채널/PID를 감독 상태로 인계한다.
4. 부모 run_parent_http_loop()는 단일 listener의 accept→FIFO→worker-policy→부분 SCM_RIGHTS 전달→CLOSED를 관리한다. 작업자는 create_worker_http_reactor() 후 advance_worker_http_reactor()를 반복한다.
5. CGI 요청은 기존 http_select_route() 검증 후 worker_file_start_response() → start_worker_file_request(prepare_cgi_launch)로 연결된다. path/query는 job 소유 복사본이다. helper가 기존 cgi_resolve_script()와 실행 환경을 준비한다. path/MIME 정책 확대는 하지 않는다.
6. helper 완료 → worker_cgi_start(): 이벤트 루프에서 CLOEXEC 출력 pipe/fork를 만들고 부모 읽기 끝만 non-blocking으로 설정한다. /dev/null과 실행 인자/env는 미리 준비한다. 자식은 restore_exec_child_signals(), close/dup2/fcntl/execve/_exit만 호출한다. malloc/free/stdio/새 파일 경로 탐색을 fork 자식에서 하지 않는다. 임의 CGI는 exec 후 자기 코드에서 라이브러리를 사용한다.
7. worker_cgi_advance()는 epoll로 CGI stdout을 읽는다. 기존 cgi_find_header_end()/cgi_parse_headers()를 재사용한다. 8KiB 헤더·Content-Type 필수·선택 Status·LF/CRLF 규칙과 5초 제한을 유지한다. 상태/Content-Type을 HTTP 헤더로 만들고 바이너리 데이터를 chunked 조각으로 송신한다.
8. 연결당 CGI 버퍼 16KiB/헤더 8KiB를 필요할 때 할당한다. 현재 조각이 소켓으로 모두 송신되기 전 CGI 파이프 읽기를 epoll에서 제거한다. 느린 클라이언트 때문에 무제한 버퍼를 만들지 않는다. 읽기 재개 시 기존 5초 읽기 제한을 적용한다.
9. worker_cgi_service()가 해당 PID만 waitpid(WNOHANG)로 회수한다. 다른 모듈이 CGI PID를 회수하지 않는다. 파이프 EOF와 exit 0 둘 다 확인한 뒤에만 마지막 0 chunk를 보낸다. 비정상 종료/잘못된 헤더는 기존 502이며 최종 응답 송신 후에는 새 오류 응답을 붙이지 않는다.
10. 클라이언트 취소/오류는 worker_cgi_cancel(): pipe 닫기→SIGTERM→기존 200ms 뒤 생존 시 SIGKILL이다. 자식 회수와 launch job cleanup 전에는 슬롯 재사용/CLOSED를 보류한다. 네트워크 실행 중 sleep/블로킹 waitpid로 다른 연결을 막지 않는다. 종료 경계에서만 기존 유예 후 동기 회수를 수행한다.
11. 부모는 기존 2초 worker 종료 유예·waitpid 확인·FD/연결/공유 예약 회수 순서를 유지하고, 자식 전원 회수 증명 후에 공유 mutex/mapping을 파괴한다. 시작 실패 시 이미 생성된 작업자를 같은 감독 종료 경로로 회수한다. 진단은 고정 숫자 필드만 기록하며 본문/민감 입력을 출력하지 않는다.

## 파일과 코드 책임

- src/process_http_server.c/h: 시작 자원 조립·fork·자식 실행·부모 감독·정리. 소켓/파서 구현을 복제하지 않는다.
- src/worker_cgi_io.c: CGI pipe/PID/epoll/청크/취소 수명. FD는 해당 worker가 소유한다.
- src/handlers/cgi_protocol.c/h: 기존 경로·헤더·환경 생성의 공유 함수. 순차 handler도 같은 정책을 사용한다.
- src/handlers/cgi_launch.c: helper에서 실행 준비/소유 메모리 정리. CGI 실행/네트워크 없음.
- worker_file_io: path 외 query도 복사하여 준비 작업이 연결 버퍼 수명을 빌리지 않게 했다.
- 이벤트 토큰은 하위2bit로 client/backend/CGI를 구분하고 기존 slot 세대를 유지한다.

## 컴파일·최소 실행 결과

- strict C11/Wall/Wextra/Werror/O2 제품과 worker, 최소 실행 바이너리 컴파일·링크 성공.
- 처음 제품 링크에서 Makefile의 소스 변수 정의 순서 때문에 일부 객체의 prerequisite가 빠지는 실패를 발견했다. 정의를 대상 선언 앞으로 이동해 수정했고 제품 링크 성공을 확인했다.
- 중간 CGI 테스트 작성은 자동 승인 검토 사용량 제한 때문에 실행되지 않았다. 사용자 재개 후 실제 파일 생성/컴파일/실행을 진행했다.
- smoke-file-io 통과: 느린 helper 중 호출자 진행·bounded 작업·정리 FD.
- smoke-process-http 통과: 실제 listener1/worker4/C backend, 느린 조회 중 정상 요청/정적 binary/빈파일/404, 종료 장부/예약/FD.
- smoke-process-server 통과: 실제 build/server main→worker4, hello/echo/greet/정적128KiB/제출/조회, CGI binary/status/query/chunk, 잘못된 출력/exec 실패/비정상 exit 시 불완전 응답 판별. 느린 CGI를 gate로 멈춘 동안 RR 4회로 모든 worker의 hello 진전, 클라이언트 RST 취소 후 CGI 회수와 각 worker FD 기준선 복귀, CGI가 살아 있는 상태의 SIGTERM 후 worker/CGI 모두 회수.
- fixture는 /tmp 임시 디렉터리만 사용한다. Python은 검증 driver이며 backend/CGI는 C다. 실제 public이나 개인 build/proxy.conf.example은 수정하지 않았다.
- 로그: build/iteration5-capability/cgi-product-build.log, cgi-product-minimal.log. 이번 통합 실행은 일반 O2이며 sanitizer 결과로 표현하지 않는다.
- 전체 make test/make verify·TSan·광범위 사망주입·부하/성능은 미실행이다. 이전 구성요소의 20개 통과 결과를 새 제품 main의 회귀 결과로 간주하지 않는다.

## 알려진 제한과 다음 체크포인트

- 기존 CGI와 마찬가지로 stdout을 닫고 계속 살아 있는 CGI는 exit 확인까지 연결 슬롯을 점유할 수 있다. 기존 blocking waitpid는 제거해 다른 연결의 실행은 계속하지만, EOF 이후 별도 전체 자식 종료 기한은 아직 추가하지 않았다. 신규 정책으로 별도 합의할 후보다.
- worker가 강제 사망한 경우 그 worker의 CGI 후손까지 감독하는 보장은 이번 최소 확인에 포함되지 않는다. 프로세스 그룹/부모 사망 처리·반복 장애는 후속 설계/장애 검증 후보다.
- 파일 helper가 막힌 disk syscall에서 join을 기다릴 수 있고, 커널 D 상태에서는 SIGKILL도 즉시 waitpid 완료를 보장하지 않는다.
- 초기화/fork 실패, 공유 fatal, 중단된 CGI framing/역압력/기한 조합, worker 사망 중 CGI, 긴 CGI Content-Type·8KiB 헤더 경계는 광범위 체크포인트 후보로 남긴다.
- 이제 실행 가능한 전체 흐름이 있으므로 전체 make test(ASan/UBSan), make verify 회귀, 공유 상태·작업자·CGI 장애 주입 범위를 묶어 검토한다. 성능은 지표/조건을 별도 합의하고 허락받기 전 실행하지 않는다. 1만 동시 연결 처리 능력이나 최종 분배 알고리즘은 확정하지 않는다.
