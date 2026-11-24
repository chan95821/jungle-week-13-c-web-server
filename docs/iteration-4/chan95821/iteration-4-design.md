# Iteration 4 Reverse Proxy 설계

외부 동작은 [개인 요구사항](iteration-4-requirements.md)을 따른다. 아래 신규 이름은 구현 흐름을 설명하며, 기존 카페·CGI 경로는 변경하지 않는다.

```text
http_select_handler() → 기존 router_handler 계약
  ├─ prepare_submission_proxy()
  │   └─ prepare_proxy_response(request, 제출 서버 설정, response)
  └─ prepare_submission_status_proxy()
      └─ prepare_proxy_response(request, 조회 서버 그룹 설정, response)

prepare_proxy_response()
  ├─ build_proxy_request_headers()
  ├─ acquire_proxy_backend()       후보 선택·연결·송신 전 우회
  ├─ send_proxy_request()          요청 헤더·본문 송신
  └─ receive_proxy_response()      최종 응답 헤더 검증·응답 준비

send_prepared_response() → 클라이언트 송신
```

현재 `prepare_proxy_response()`의 위 호출 순서는 코드에 연결했다. `acquire_proxy_backend()`는 설정 검사, 후보 요청, 연결, 정책에 결과 통보, 제한 횟수 내 우회를 조율한다. 한 서버 연결은 `connect_backend()`로 위임한다. 요청 송신, 응답 헤더 수신·검증, 본문 공급자 준비까지 구현했다. 남은 미구현은 아래 `남은 범위`에 정리한다.

`connect_backend()`는 숫자 IPv4/IPv6만 받는다(DNS 제외). `prepare_backend_address()`로 접속 주소를 준비하고 소켓을 만든 뒤, `set_fd_close_on_exec()`와 nonblocking 설정을 적용해 연결을 시작한다. 진행 중이면 `wait_backend_connection()`이 공통 `wait_fd_ready()`로 `POLLOUT`을 기다리고 `SO_ERROR`로 실제 결과를 확인한다. 성공 시 blocking 모드로 돌린 FD를 호출자에게 넘기며 실패 시 소켓을 내부에서 닫는다. 연결 제한은 검증 기본값 1초다.

`src/io_utils.c`는 기존 CGI의 마감 계산·대기와 서버의 FD 상속 방지 설정을 공유한다. CGI는 같은 5초 제한으로 `POLLIN`을 기다린다. `FD_CLOEXEC`는 exec 시 닫힘을 보장하며, `socket()` 뒤 `fcntl()`로 설정하는 현재 방식의 스레드/exec 경합은 Iteration 5에서 재검토한다.

`send_proxy_request()`는 `make_io_deadline()`으로 한 번 만든 마감 시각을 두 `send_all_bytes()` 호출에 공통으로 넘긴다. 헤더를 먼저 완전 송신한 뒤 본문을 보내며, 개인 검증 기본값은 합계 1초다. 이 함수는 FD를 빌릴 뿐 닫거나 우회하지 않는다. 오류는 상위 함수가 정리하고 클라이언트 응답으로 바꾼다. `send_all_bytes()`는 기존 서버 `send_bytes()`의 부분 송신·`EINTR` 처리와 소켓 타임아웃·취소 검사를 `io_utils.c`로 이동한 공통 구현이다. 클라이언트 송신도 이 구현을 사용한다. 마감이 있는 송신은 마지막 부분 송신 뒤에도 총시간 초과를 확인한다.

요청 본문 스트리밍은 추후 고려 사항이다. 현재 메모리 버퍼링과 본문 상한을 유지하며, 큰 업로드 또는 동시 요청 메모리 사용이 실제 요구·측정으로 문제가 되면 입력 수신 계약부터 재검토한다.

`acquire_proxy_backend()`의 `available_for_this_request`는 이번 요청에서 아직 시도하지 않은 후보만 표시하는 임시 메모리다. 그룹의 제외 만료 시각은 공통 프록시가 읽거나 갱신하지 않는다.

조회 정책은 `src/proxy_selection.c`에서 두 함수로 제공한다. `select_available_round_robin_backend()`는 요청별 목록에 제외 기간을 적용하고 기존 순수 `select_round_robin_backend()`로 후보를 고른다. `record_backend_connection_result()`는 성공 시 복귀, `502`·`504` 연결 실패 시 2초 제외, 다른 오류 시 상태 유지를 담당한다. 공통 프록시는 `on_connection_result` 콜백으로 인덱스·상태 코드만 통보한다. 정책 함수는 FD를 받거나 닫지 않는다.

결과 기록 함수는 정상 처리 시 0, 내부 실패 시 500을 반환한다. 기록에 실패했는데 연결이 성공해 FD가 열려 있다면 `acquire_proxy_backend()`가 닫고 FD를 -1로 되돌린다. 호출자는 오류 응답을 준비하며 해당 연결로 요청을 보내지 않는다.

`acquire_proxy_backend()`가 성공하면 호출자가 백엔드 FD를 소유한다. 이후 오류는 `prepare_proxy_response()`에서 닫는다. `receive_proxy_response()`가 성공하면 FD를 본문 공급자에 넘기거나 본문이 없는 응답의 FD를 닫는다. 그 후 공통 함수는 FD를 다시 닫지 않는다.

라우트별 핸들러는 백엔드 주소 또는 서버 그룹과 선택 정책을 전달한다. 공통 프록시는 `http_request.target`을 그대로 중계하지만 경로를 정책 선택에 사용하지 않는다. 사전 헤더 검사는 요청 본문을 읽기 전에 수행하며 이 호출 트리의 본문 수신 후 처리와 구분한다.

순수 `select_round_robin_backend()`는 후보 목록과 순번만 다룬다. 이를 사용하는 조회 정책이 제외·복귀를 관리한다. 소켓 연결·정리는 정책의 책임이 아니다. 고정 주소 경로는 선택 정책을 호출하지 않는다. 탑다운으로 `routes[]`의 두 경로와 핸들러를 먼저 연결한다. 실제 중계가 없는 상태에서는 오류를 반환하며 성공 응답을 흉내 내지 않는다.

## 공통 프록시 입력

`prepare_proxy_response()`는 기존 `const struct http_request *request`와 `struct router_response *response` 사이에 `const struct proxy_upstream *upstream`을 받는다. 라우트 핸들러는 기존 시그니처를 유지하고 [설정 모듈](#설정-파일과-라우트-설정)이 확정한 업스트림을 넘긴다.

| 항목 | 계약 |
| --- | --- |
| `request` | 검증한 요청을 빌려 읽는다. `target`은 백엔드 요청을 작성할 때만 쓰고 목적지 선택에 사용하지 않는다. |
| 백엔드 설정 | 접속 주소 목록, 선택 함수와 결과 기록 함수(선택 사항), 연결 시도 상한을 전달한다. 선택 함수가 없으면 주소는 한 곳이며 시도 상한은 1회다. 조회는 A·B와 두 정책 콜백, 시도 상한 2회를 전달한다. |
| `response` | 기존 `router_handler` 계약과 같이 호출자가 제공한 `router_response`를 채운다. 클라이언트 소켓을 인자로 받지 않는다. |

조회 그룹의 순번과 서버별 제외 시각은 요청이 끝나도 유지해야 하는 상태다. `proxy_group_state`의 정의는 `src/proxy_selection.h`에 두고, 공통 프록시에는 불완전 타입의 포인터만 제공한다. 실제 저장소는 `src/proxy_config.c`의 정적 배열이며 두 정책 콜백에 같은 참조를 전달한다. 고정 주소에는 이 상태가 필요 없다.

## 라우트와 핸들러

라우트마다 핸들러를 따로 둔다. 핸들러 이름이 시나리오와 목적지 그룹을 함께 드러내며, 공통 중계 규칙은 모두 `prepare_proxy_response()`가 맡는다.

| 경로 | 핸들러 | 목적지 |
| --- | --- | --- |
| `/api/hello` | `prepare_hello_proxy()` | 설정의 단일 백엔드 (부모 이슈 #4 중계 확인용 더미) |
| `/api/submissions` | `prepare_submission_proxy()` | 설정의 제출 백엔드 |
| `/api/submission-status` | `prepare_submission_status_proxy()` | 설정의 조회 그룹(A·B + 선택·제외 정책) |

## 라우트 선택과 사전 검증

`route_rule.validate_request`는 선택적인 검증 함수다. 기존 라우트는 `NULL`, 두 프록시 라우트는 `validate_proxy_headers()`를 등록한다. `select_rule()`의 매칭이 성공한 뒤 `http_select_handler()`가 검증 함수를 호출한다. 검증에 실패하면 오류 상태와 빈 핸들러를 반환한다. 기존 선택 API와 `handle_client()`의 호출 순서를 유지하므로 `100 Continue`와 추가 본문 수신 전에 오류 응답으로 분기한다.

`validate_proxy_headers()`와 요청 헤더 작성은 `src/proxy_validation.c`의 `connection_lists_header()`를 공유한다. 검사 단계에서는 I/O나 원본 헤더 변경을 하지 않는다.

## 백엔드 응답을 기다리는 위치

현재 `receive_proxy_response()`는 `make_io_deadline()`과 공통 `receive_http_headers()`로 첫 헤더 블록을 읽는다. 전체 수신 길이와 헤더 끝 위치로 헤더와 동반 본문을 구분해 `prepare_backend_response()`에 넘긴다. 요청과 응답은 같은 헤더 경계 탐색·바이트 수신 유틸을 사용한다.

`prepare_backend_response()`는 `http_parse_response_headers()`로 상태·필드 문법과 길이를 해석하고 `validate_backend_response_headers()`로 고정 길이 중계 범위를 검증한다. 해석용 `http_response_headers`는 입력을 빌리는 메타데이터이며 송신용 `router_response`를 확장하는 것이 아니다. 요청·응답의 헤더 한 줄 문법과 숫자 길이 검사는 `http.c`에서 공유한다.

해석·검증에 성공한 결과는 `prepare_backend_body()`로 넘긴다. 이 함수는 Content-Type·전달 대상 헤더·동반 본문을 한 할당으로 복사해 응답 소유 저장소를 만들고, 선언 길이의 나머지는 `proxy_backend_body_next()`가 백엔드 소켓에서 읽는다. 본문 수신은 조각마다 새 무진행 제한(검증 기본값 1초)을 두므로 느리지만 계속 오는 본문은 끊지 않고, 멈춘 본문만 `BODY_ERROR`가 된다. 성공하면 FD 소유권이 공급자로 넘어가고 `proxy_backend_body_cleanup()`이 송신 성공·실패 모두에서 FD를 닫고 메모리를 해제한다.

중간 `1xx`를 건너뛰고 최종 응답까지 계속 읽는 루프가 아직 없으므로 `validate_backend_response_headers()`가 `1xx`를 `502`로 거부한다. 1xx를 최종 응답처럼 잘못 중계하지 않기 위한 임시 경계이며, 정책 판단은 검증 함수 한 곳에만 둔다.

## 설정 파일과 라우트 설정

`./build/server <port> [routes.conf]`로 실행하며, 파일을 생략하면 내장 기본값(제출 `8001`, 조회 `8002`·`8003`)을 쓴다. `src/proxy_config.c`가 파일을 읽어 라우트별 `proxy_upstream`을 확정하고, 라우트 핸들러는 `proxy_config_submission()`·`proxy_config_lookup()`이 돌려주는 포인터를 그대로 넘긴다.

- `submit <host>:<port>`는 정확히 한 줄, `lookup <host>:<port>`는 한 줄 이상(최대 `PROXY_LOOKUP_MAX_BACKENDS`)이다. 빈 줄과 `#` 주석 줄은 무시하고 IPv6는 `[::1]:8002`로 쓴다.
- 주소는 숫자 IPv4·IPv6만 허용한다. 숫자 검증과 포트 검증을 설정 단계에서 끝내므로 연결 단계에서는 주소 형식 오류가 나오지 않는다.
- 파싱은 임시 `struct parsed_routes`에 모으고, 파일 전체가 유효할 때만 확정 저장소로 복사한다. 따라서 형식 오류가 나면 직전 설정이 그대로 유지되고 서버는 시작하지 않는다.
- `proxy_group_state`의 제외 시각 배열은 확정 저장소의 정적 배열이며, 설정을 다시 읽을 때 초기화된다.

`router_response`에는 `headers`·`headers_length`를 추가해 프록시가 준비한 응답 헤더 블록을 넘긴다. 기존 핸들러는 두 값을 비워 두므로 출력이 바뀌지 않는다. 프록시는 hop-by-hop 헤더, `Connection`이 지목한 필드, 다시 작성하는 `Content-Length`·`Content-Type`을 제외한 나머지를 원본 순서와 다중 줄(`Set-Cookie`) 그대로 보존한다. `send_response()`는 상태 줄·Content-Type을 먼저 보내고 준비된 헤더를 이어서 보낸 뒤, 마지막에 길이·연결 헤더를 붙여 중복을 만들지 않는다. `204`·`304`는 길이 없이 헤더만 보내고 본문을 생략한다.

## 남은 범위

- 중간 `1xx` 무시 후 최종 응답까지 같은 제한 안에서 계속 읽기
- `HEAD`(현재 라우터가 `501`), `chunked` 응답 중계, 연결 종료로 끝나는 본문
- 응답 방향 `Via` 추가. 요청 방향 `Via`만 넣고 있다.
- 정적 파일 루트를 설정 파일/CLI로 받기(현재 `public` 고정)

- `handle_client()`의 기존 `respond:` 분기에서 호출되는 프록시 핸들러가 `prepare_proxy_response()`로 백엔드 요청을 보내고, 최종 응답 헤더를 기다려 검증한다. `respond:` 진입 전에는 백엔드 응답을 기다리지 않으며, 핸들러가 돌아올 때는 클라이언트에 보낼 정상 또는 오류 응답이 준비되어 있어야 한다.
- `send_prepared_response()`는 준비된 상태와 헤더를 먼저 보내고 `body_provider`를 통해 나머지 백엔드 본문을 필요할 때 읽어 중계한다. 전체 본문을 미리 기다리지 않는다. 백엔드 응답 헤더와 같은 읽기에 들어온 본문 바이트가 있다면 이를 먼저 중계한다.
- 최종 헤더 대기 제한(검증 기본값 3초)을 넘기면 최종 응답을 보내기 전이므로 `504`를 준비한다. 최종 응답 송신이 시작된 뒤 본문 중계가 실패하면 새 오류 응답을 보내지 않고 연결을 정리한다.
