# B 구현을 요청 순서대로 읽기

## 가독성 정리 후 먼저 볼 흐름

현재 코드는 같은 동작을 유지하면서 긴 조율 함수를 기능별로 나눴다. 아래 함수들은 모두 프로젝트가 직접 구현한 함수다. `pthread_create`, `epoll_wait`, `recv`, `sendmsg` 등은 시스템/라이브러리 API다.

메인에서는 스레드 준비·시작·종료가 분리되어 있다.

~~~text
run_event_server
  → initialize_network_workers   : 작업자별 epoll/eventfd 준비
  → start_local_work_pool        : 파일/CGI 보조 풀 시작
  → start_network_workers        : 네트워크 스레드 시작
  → accept_connections           : 메인이 수락·분배 반복
  → stop_network_workers         : 종료 알림 후 join
  → 보조 풀과 나머지 자원 정리
~~~

작업자의 한 바퀴는 `run_worker_event_loop`에서 읽는다.

~~~text
receive_worker_inbox             새 연결/보조 결과 받기
process_ready_connections        메모리에서 바로 진행할 연결 처리
collect_retired_connections      안전한 연결만 최종 해제
expire_due_connections           기한 만료 처리
epoll_wait                      다음 소켓/제어 알림 대기
process_worker_events            각 연결 재개
collect_retired_connections      배치가 끝난 뒤 해제
~~~

연결 하나는 `advance_connection`에서 종료·기한·budget을 확인하고, `advance_current_phase`로 현재 일을 진행한다. 클라이언트 출력은 `advance_client_output`이 담당하며, **모두 보낸 경우에만** `continue_after_client_output`이 다음 단계로 전환한다. 백엔드 데이터가 진행됐다는 결과 역시 유지하므로 메모리에 남은 헤더·본문을 다음 이벤트 없이 이어 처리한다.

| 송신 결과 | 의미 | 다음 행동 |
|---|---|---|
| `SEND_FAILED` (-1) | 송신 오류 | 연결 정리 |
| `SEND_WOULD_BLOCK` (0) | 지금 보낼 수 없음 | epoll 알림 대기 |
| `SEND_FINISHED` (1) | 준비한 조각 전체 송신 | 다음 단계/producer로 이동 |
| `SEND_CAN_CONTINUE` (2) | 부분 송신 또는 EINTR | 현재 송신 위치부터 재시도 |

백엔드 처리는 `start_backend_request`의 주소 선택 → 버퍼 준비 → 접속으로 읽는다. 본문은 `read_backend_response_body`가 프레이밍을 고르고, `read_chunked_body`는 청크 단계, `read_backend_payload`는 실제 바이트 전달을 담당한다.

보조 스레드는 `take_next_local_job` → `notify_local_queue_space` → `execute_local_job` → `publish_local_job_result`를 반복한다. 큐 lock은 수령/공개에만 사용하며 파일/CGI 실행 구간에는 잡지 않는다.

## 작업자, epoll, 이벤트 루프의 관계

작업자는 pthread로 만든 **스레드**다. 이 프로젝트는 네트워크 작업자를 시작할 때 정해진 개수만 만들고 재사용한다. 요청마다 새 스레드를 만들지 않는다.

epoll은 메모리 공간이나 스레드가 아니다. Linux 커널에 “이 FD들에서 읽기/쓰기가 가능해지면 알려 달라”고 등록하는 **감시 인스턴스**다. `epoll_create1()`이 그 인스턴스를 가리키는 FD를 반환한다. 작업자마다 하나를 만들고 자기 연결들의 client/backend FD를 등록한다.

요청 본문·버퍼·단계·송신 위치는 epoll에 저장하지 않는다. `struct event_connection`에 저장한다. `epoll_event.data.ptr`는 해당 연결의 `event_endpoint`를 찾는 포인터일 뿐이다. 프로세스 하나이므로 모든 스레드는 같은 주소 공간과 FD 테이블을 공유한다. 소유권은 “수정·정리는 이 작업자만 한다”는 코드의 약속이다.

이벤트 루프는 다음을 반복한다.

~~~text
도착한 연결/보조 결과 수령
→ 이미 메모리에 있는 처리 가능한 일 진행
→ 만료된 연결 처리
→ epoll_wait(가장 가까운 만료까지)
→ 준비된 FD의 연결 찾아 가능한 만큼 진행
→ 반복
~~~

`recv()`가 `EAGAIN`을 반환하면 “지금 읽을 바이트가 없다”다. 실패 응답을 만들지 않고 다음 EPOLLIN까지 다른 연결을 처리한다. `sendmsg()`의 EAGAIN도 다음 EPOLLOUT을 기다린다. 각 연결에는 다음에 이어 할 위치가 남는다.

## 먼저 볼 파일

| 파일 | 책임 |
|---|---|
| `src/main.c` | 인자/설정/루트 확인, listener 생성, B 서버 호출 |
| `src/event_server.c` | 고정 스레드 준비, accept, 연결 분배, 신호와 join |
| `src/event_worker.c` | worker별 epoll/eventfd/큐, 타이머 힙, 이벤트 루프 |
| `src/event_connection.c` | 클라이언트 수신·송신, 연결 단계 조율, 정리 |
| `src/event_proxy.c` | nonblocking 백엔드 연결·요청 송신·응답 해석 |
| `src/local_work.c` | 파일/CGI의 대기 가능한 작업을 보조 풀에서 실행 |
| `src/event_internal.h` | 연결 상태와 각 필드의 소유권 |

기존 `http.c`는 HTTP 파싱, `router.c`는 경로/메서드 선택, `proxy.c`는 설정과 A/B 선택을 계속 담당한다. `proxy_headers.c`/`proxy_body.c`의 순수 생성·검증 함수는 이벤트 경로에서도 재사용한다. 같은 헤더 정책을 두 벌로 새로 만들지 않기 위해서다.

## 1. 서버 시작

`main()` → `parse_port()` → `proxy_load_config()` → `stat(public)` → `open_listenfd()` → `run_event_server()`.

`run_event_server()`가 SIGINT/TERM을 스레드 생성 전에 차단하고 signalfd를 만든다. 자식 스레드도 차단 상태를 물려받는다. 따라서 어느 worker에서 임의의 신호 핸들러가 연결을 정리하는 일이 없다. 관리자가 signalfd로 신호를 확인하고 각 작업자에게 종료를 요청한다.

각 작업자의 `initialize_event_worker()`는 epoll FD, 깨우기용 eventfd, inbox mutex를 준비한다. 모든 작업자의 알림 FD를 먼저 만든 뒤 보조 풀 2개와 네트워크 작업자 N개를 시작한다. config는 마지막 join까지 살아 있다.

## 2. 학생이 접속

관리자의 `accept_connections()` → `accept4(...SOCK_NONBLOCK | SOCK_CLOEXEC)` → `hand_client_to_worker()` → `assign_client_to_worker()`.

부하가 적은 작업자부터 선택한다. 선택한 작업자의 상한 있는 큐에 FD와 accept 시간을 넣고 eventfd에 1을 쓴다. 이때 FD 소유권이 관리자로부터 작업자로 넘어간다. 실패하면 관리자가 닫는다. 나중에 두 곳이 같은 FD를 닫지 않도록 경계를 명확히 둔다.

eventfd는 HTTP 데이터를 전달하지 않는다. “inbox에 확인할 일이 있다”는 깨우기 알림이다. 실제 FD·결과는 mutex로 보호한 큐에 있다.

## 3. 작업자가 연결을 준비

`run_worker_event_loop()` → `receive_worker_inbox()` → `create_event_connection()` → `schedule_connection()` → `advance_connection()`.

연결 하나에 구조체 하나를 할당한다. client FD, backend FD(-1), 헤더 버퍼, 본문 포인터, 단계, 기한, 출력 위치가 들어 있다. 요청 헤더 slice가 이 구조체의 버퍼를 빌리므로 함수가 반환된 뒤에도 문자열 포인터가 유효하다.

처음 단계는 `READ_CLIENT_HEADER`다. 헤더 전체 기한은 worker가 집어 든 시간이 아니라 **accept된 시간 +7초**다. 큐에서 기다린 시간도 포함한다. 기존 호출별 대기 제한에 대응하는 **입력 idle 5초**도 별도로 적용한다. 양쪽 중 빠른 기한이 먼저 만료된다.

## 4. 요청 헤더와 본문

`advance_connection()` → `read_client_request_headers()` → CRLFCRLF 탐색 → `process_received_request_header()`.

- 헤더 끝이 아직 없으면 저장한 끝에서 다음 recv를 이어 한다. 8KiB가 차면 431이다.
- `http_parse_request()`가 메서드·경로·HTTP 버전·Host·Content-Length 등을 검증한다.
- `http_select_route()`가 로컬/제출/조회를 선택한다. 잘못된 메서드를 고쳐 보내지 않고 405/501 등을 만든다.
- 프록시 경로면 `proxy_validate_request()`로 보호해야 할 Connection 지정을 확인한다.
- 필요한 만큼만 본문 메모리를 할당하고 헤더와 함께 도착한 바이트를 먼저 복사한다.
- 필요할 때 `SEND_CONTINUE`에서 100 Continue를 보낸 다음 `READ_CLIENT_BODY`로 돌아온다.
- 본문도 부분 수신을 누적하며, 미완성인데 EOF면 400, 기한 초과면 408이다.

본문을 다 받은 뒤 `dispatch_complete_request()`가 목적지를 결정한다. 경로/헤더 오류는 본문 대기·100 Continue·백엔드 접속보다 먼저 판정한다.

## 5. 제출/조회 백엔드 결정

프록시 경로면 `start_backend_request()`를 호출한다.

~~~text
POST /api/submissions         → submission_backend
GET /api/submission-status    → query A/B
GET /api/hello                → 같은 query A/B 순번
~~~

공유 `selection_mutex` 안에서 기존 `proxy_select_backend()`만 호출하고 바로 lock을 푼다. 조회를 선택한 순서는 A/B/A/B다. 접속 실패도 순번을 소비하고 재시도하지 않는다. 제출·로컬·잘못된 요청은 조회 순번을 소비하지 않는다. 병렬 처리이므로 응답이 완료되는 순서는 교대한다고 보장하지 않는다.

“어느 작업자가 맡나”와 “어느 조회 백엔드로 보내나”는 서로 다른 선택이다.

## 6. 백엔드에 접속하고 요청 송신

`build_backend_request_header()`가 기존 정책대로 헤더를 만든다. raw target/query를 그대로 쓰고, Host는 선택한 백엔드 주소로 바꾸고, hop-by-hop 필드를 제거하고, Content-Length/Connection close/Via를 구성한다. 본문은 바이트 길이로 보낸다.

socket을 NONBLOCK/CLOEXEC로 생성하고 `connect()`를 호출한다. 즉시 성공하면 `SEND_BACKEND_REQUEST`, EINPROGRESS면 `CONNECT_BACKEND`다. EPOLLOUT을 받은 뒤 `getsockopt(SO_ERROR)`로 실제 성공 여부를 확인한다. “쓸 수 있다” 알림만으로 접속 성공이라고 생각하면 안 된다.

`send_pending_output()`는 최대 세 조각을 `sendmsg()`로 보낸다. header/body를 합치려고 본문을 복사하지 않는다. 덜 보내면 `output.index/offset`에 위치를 저장한다. EAGAIN이면 돌아온다. 연결 1초, 요청 송신 5초의 절대 기한은 이벤트마다 연장하지 않는다.

## 7. 백엔드 응답 헤더

요청 송신이 끝나면 `READ_BACKEND_HEADER` → `read_backend_response_headers()`다. 기한은 1초다.

응답 조각을 16KiB 버퍼에 받고 완성된 헤더 블록을 `consume_backend_header_block()`로 넘긴다. 이 함수는 기존 `parse_backend_headers()`와 `build_client_response_headers()`를 재사용한다. 100은 다시 보내지 않고, 다른 지원 1xx는 최종 헤더 검증까지 보류한다. 101·잘못된 길이/TE 등은 거절한다.

최종 헤더가 검증되면 `SEND_CLIENT_HEADER`가 된다. 이때 응답 상태·reason·전달 대상 헤더는 기존 규칙대로 보존된다. 접속 거절 등은 502, 응답 없는 기한 초과는 504, 할당 실패는 500이다.

## 8. 응답 본문과 역압

`SEND_CLIENT_HEADER` → `READ_BACKEND_BODY` → `SEND_CLIENT_BODY` → 다시 `READ_BACKEND_BODY`를 반복한다.

- Content-Length: 남은 정확한 바이트 수만 전달한다. 조기 EOF는 불완전 응답이다.
- chunked: 크기 줄/데이터/뒤 CRLF/트레일러의 단계를 별도로 저장한다. 한 글자씩 도착해도 이어 해석한다.
- EOF: 정상 EOF까지 받은 조각을 클라이언트에 chunked로 전달한다.
- 본문 없음: 본문을 기다리지 않고 종료 단계로 간다.

받은 조각은 stream 버퍼를 빌린 포인터다. 클라이언트에 다 보내기 전에는 다음 recv나 provider.next를 호출하지 않는다. 클라이언트가 느리면 EPOLLOUT을 기다리는 동안 백엔드 EPOLLIN을 해제한다. 버퍼를 늘리는 대신 TCP 수신 흐름 제어로 생산 속도를 낮춘다.

헤더/본문 조각/끝 chunk는 모두 partial send 위치를 저장한다. chunked의 마지막 `0 CRLF`와 트레일러는 정상 BODY_END에서만 만든다. 본문 중단 시 추가 오류 응답을 붙이지 않고 연결을 닫아, 클라이언트가 고정 길이 부족이나 chunk 종료 누락을 판별하게 한다.

## 9. 기존 로컬 경로

로컬 경로는 기존 handler를 사용한다. hello/echo/greet의 짧은 메모리 처리는 네트워크 작업자가 한다. 파일/CGI 준비는 `submit_local_job(PREPARE_LOCAL_RESPONSE)`로 보조 풀에 맡긴다.

보조 스레드가 완료 결과를 owner의 inbox에 넣고 eventfd로 깨운다. 본문 provider의 읽기/정리도 같은 방식이다. 한 연결에는 보조 작업이 하나만 진행되고, 그 작업이 끝나기 전에는 owner가 provider를 만지거나 연결 메모리를 해제하지 않는다.

CGI는 요청에 맞는 외부 프로그램을 실행하고 stdout을 파이프로 읽어 응답하는 방식이다. `posix_spawn()`을 사용해 멀티스레드 fork 직후의 libc 사용 문제를 피한다. 파일·socket·pipe에는 CLOEXEC을 원자적으로 지정하여 CGI 실행 파일에 네트워크 FD가 남지 않게 한다. stdout만 닫고 계속 실행되는 CGI의 자식 회수 대기도 제한한다.

이는 네트워크 요청 하나를 통째로 blocking worker에 맡기는 C 모델이 아니다. 네트워크는 계속 epoll 상태 머신으로 처리하며, 일반 파일 I/O는 epoll로 준비 상태를 감시할 수 없어서 보조 풀을 둔다. 비교할 때 이 보조 스레드 수도 명시해야 한다.

## 10. 정상 완료·오류·서버 종료

성공하면 `begin_client_drain()`이 backend FD를 닫고 client에 SHUT_WR한다. 이후 `DRAIN_CLIENT`에서 남은 입력을 최대 1초/64KiB까지 이벤트로 읽고 닫는다. client의 SHUT_WR/RDHUP는 “요청 입력 끝”일 수 있으므로 곧바로 응답까지 포기하지 않는다.

`close_event_connection()`은 epoll 등록 제거 → FD close → 필요하면 보조 cleanup → `retire_connection()` 순서다. 메모리는 같은 이벤트 배치를 모두 처리한 뒤 해제한다. 같은 epoll_wait 결과 배열에 client/backend 이벤트가 함께 있을 수 있어, 첫 이벤트에서 즉시 free하면 다음 이벤트가 해제된 포인터를 볼 수 있기 때문이다.

SIGINT/TERM이면 수락을 중지하고 작업자를 깨운다. 작업자는 연결을 닫고 대기 중 보조 작업은 취소한다. 이미 실행 중인 파일/CGI 작업의 결과와 cleanup까지 회수한 뒤 종료한다. 네트워크 worker join 다음 보조 풀 join이며, config와 mutex는 마지막에 해제한다. 임의의 디스크 커널 I/O 정지를 즉시 취소한다고 보장하지 않는다.

## CSAPP과 이어 보기

| 구분 | 예 |
|---|---|
| POSIX/libc가 제공 | socket/connect/recv/sendmsg/close, pthread_create/mutex/join, posix_spawn |
| Linux가 제공 | epoll_create1/epoll_ctl/epoll_wait, eventfd, signalfd, accept4 |
| 프로젝트가 구현 | run_event_server, advance_connection, read_backend_response_headers, send_pending_output |
| CSAPP 관례 이름인 프로젝트 함수 | open_listenfd, send_all |

CSAPP의 동시 서버에서 I/O multiplexing과 thread pool을 결합한 구조다. TCP 재전송·전송 순서는 OS가 맡고, 서버는 HTTP 메시지 경계·부분 송수신 위치·상한·기한·FD 수명을 맡는다. epoll은 HTTP를 해석하거나 본문을 대신 보내주지 않는다.
