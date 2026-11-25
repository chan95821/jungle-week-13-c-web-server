# Iteration 4 실제 코드 흐름

기록 갱신: 2026-11-25. P2-B 메모리 수명 개선에 이어 P2-9 A안 라우팅 일원화를 완료했다. 단위 8개(ASan/UBSan)·E2E 스크립트 6개 통과, 엣지 40개 통과·실패 0·skip 0. 이전 결과는 단계별 보고서로 보존한다.

## 1. main: 서버 수명과 변경 가능한 상태

src/main.c의 main은 실행 인자 <port> <proxy.conf>를 확인하고 parse_port → proxy_load_config → public 디렉터리 확인 → setup_signals → open_listenfd 순서로 시작한다.

설정은 struct proxy_config, 조회 순번은 struct proxy_selection_state로 main의 지역 공간에 둔다. 종료 신호 표시 외에 새 변경 가능한 전역 상태는 없다. accept로 얻은 client_fd는 main이 소유하고 handle_client에 빌려 준다. handle_client가 돌아오면 main이 한 번 close한다.

설정 파일은 사용자가 이동한 build/proxy.conf.example을 실행 인자로 지정한다. build/ 무시 규칙과 clean의 명시적 산출물 목록 때문에 Git에 올라가지 않고 clean으로도 삭제하지 않는다.

## 2. handle_client: 요청 하나의 조율

src/server.c는 handle_client를 파일 위에 두었다.

1. receive_request_headers: 기존 8KiB 버퍼와 헤더 전체 7초 기한으로 요청 헤더와 함께 도착한 본문 일부를 보존한다.
2. http_parse_request: 기존 HTTP/1.1·origin-form·Host·Content-Length·64KiB 상한·미지원 정책을 검증한다. 일반 필드는 원본 헤더의 slice로 순회할 수 있다.
3. router.c의 http_select_route: select_rule → path_matches_rule 순서로 단일 routes[]의 로컬 8개·프록시 3개 규칙을 판별한다. 선택 결과 selected_route에는 local_handler 또는 proxy_route가 들어간다. GET/POST 외 501 → 모르는 경로 404 → 등록 경로의 잘못된 GET/POST 405·Allow 순서다. 성공한 프록시 요청에만 proxy.c의 proxy_validate_request를 호출해 보호 Connection 지명을 400으로 거절한다. 로컬 요청에는 프록시 검사를 적용하지 않는다. 이 과정은 본문 할당·100 Continue·백엔드 선택/접속보다 먼저이며 I/O나 요청 변경을 하지 않는다.
4. request_body: 검증한 길이만큼 한 번 할당하고 동반 수신분을 복사한 뒤 부족분만 받는다. 필요한 100 Continue는 프록시가 보낸다. 불완전 본문은 백엔드로 전달하지 않는다.
5. proxy_select_backend: 제출은 제출 주소, 두 조회 경로는 공유하는 A/B 순번이다. 본문 수신까지 성공한 요청에서 실제 선택하고 선택 직후 순번을 넘긴다. 재전송하지 않는다.
6. selected_route.proxy_route가 프록시 역할이면 proxy_prepare_response, LOCAL이면 selected_route.local_handler를 실행한다. 주소·Round-Robin 상태는 라우터가 소유하지 않는다.
7. send_prepared_response가 전송과 provider cleanup을 수행한다. 성공하면 기존 제한적 수신 폐기를 하고 돌아온다.
8. cleanup에서 요청 본문을 해제한다. 클라이언트 FD는 닫지 않는다.

request_header, request_body, header_end_offset, body_received_bytes, request_io, client_response_io처럼 역할과 바이트 수를 드러내는 이름으로 정리했다.

## 3. proxy_prepare_response: 백엔드 수명 시작

src/proxy_transfer.c는 연결별 상태 생성 → 준비 → 소유권 이전 → 실패 정리의 상위 흐름만 담당한다.

1. calloc으로 proxy_stream 하나를 만든다. backend_fd 초기값은 -1이다.
2. src/proxy_io.c의 connect_backend는 socket → close-on-exec → 일시적 non-blocking connect → poll/SO_ERROR → blocking 복원을 수행한다. 전체 1초이며 이벤트 루프를 구현한 것은 아니다.
3. src/proxy_headers.c의 forward_request_to_backend는 원래 메서드·target을 보존하고 Host를 백엔드 주소로 재작성한다. 필요한 Content-Length·Connection·Via를 생성하고, 일반 헤더를 유지하며 hop-by-hop/지명 헤더와 Expect는 제거한다. 길이 없는 빈 GET에는 CL: 0을 합성하지 않고 명시적 길이·POST 길이 정책을 유지한다. 요청 송신 전체 5초다.
4. 같은 파일의 prepare_backend_response_headers는 지역 proxy_response_headers에 raw_header 8KiB와 client_header 작업 공간을 마련한다. read_backend_byte → parse_backend_headers → build_client_response_headers 순으로 처리한다. 1xx마다 원본 헤더를 다시 사용하되 출력 작업 공간에는 100 외 중간 응답을 누적하며, 최종 응답 검증까지 전체 1초를 유지한다.
5. parse_backend_headers는 HTTP/1.0·1.1 응답을 받고 상태/필드/프레이밍을 검사한다. HTTP/1.0의 1xx/TE와 101 등 기존 거절 정책은 유지한다. build_client_response_headers는 클라이언트 상태 줄을 HTTP/1.1로 쓰되 상태 코드·문구를 보존한다. 반복 필드·204/304·Via·최종 응답만 close 생성하는 규칙도 그대로다.
6. 최종 chunked 응답에서만 retain_trailer_connection_fields가 모든 Connection 필드 줄을 정확한 길이로 복사한다. fields/status_line은 지역 원본을 참조하므로 스트림에 남기지 않는다. 송신 헤더도 완성된 바이트 길이만 할당·복사하고 지역 작업 공간은 반환 시 수명을 마친다.
7. 전체 본문 수신 15초 기한을 만들고 response에 wire_header와 body_provider를 넘긴다. 성공 후 백엔드 FD와 힙 공간은 provider가 소유한다. 준비 중 실패는 cleanup_backend_response로 모두 정리한 뒤 기존 500/502/504를 만든다.

## 4. send_prepared_response → read_backend_body_chunk: 송신과 단계 전환

src/server.c의 send_prepared_response는 검증된 wire_header를 먼저 완전히 보낸다. 그 다음 고정 길이는 send_body, chunked/EOF는 send_chunked_body를 사용한다.

두 송신 함수가 src/proxy_body.c의 read_backend_body_chunk를 호출하면, 이 함수는 먼저 이미 송신된 client_header를 free하고 NULL로 바꾼다. 이후에는 16KiB 수신 버퍼를 재사용해 본문 조각을 빌려 준다. wire_header 참조는 첫 next 호출 또는 cleanup 전까지만 유효하다. 본문이 없거나 헤더 송신이 실패하면 next가 호출되지 않아도 cleanup에서 회수된다.

- Content-Length: 남은 바이트만 제공한다. 조기 EOF는 실패이고 초과분은 현재 본문에 넣지 않는다.
- chunked: 크기·확장·CRLF·0 청크를 검사한다. read_trailers는 보존한 Connection 지명 정보를 사용해 기존 금지 필드를 검사한다. 트레일러는 지역 공간에서 기존 합계 8KiB 상한을 지킨 뒤, 값이 있을 때만 정확한 길이로 할당한다.
- EOF 종료형: 정상 EOF까지 제공한다. 정상 FIN만으로 백엔드가 의도한 전체 내용을 받았다고 보장하지 않는다.
- 본문 없음: 데이터를 생성하지 않는다. 무진행 5초·전체 본문 수신 15초와 종료 표시 검사를 유지한다.

공급자 정상 완료일 때만 송신 함수가 종료 청크와 검증된 트레일러를 보낸다. 송신 실패·트레일러 할당 실패·잘못된 본문에는 새 HTTP 오류나 정상 종료 청크를 붙이지 않는다.

## 5. cleanup_backend_response: 정리 책임

src/proxy_transfer.c의 cleanup_backend_response는 백엔드 FD와 남은 client_header·connection_fields·trailer_buffer·스트림 상태를 회수한다. 준비 실패 때는 준비 함수가, 성공 후에는 send_prepared_response가 정확히 한 번 호출한다. 요청 본문은 handle_client가 이후 해제하고, 클라이언트 FD는 main이 닫는다.

기존 네 배열의 합 49,408바이트(48.25KiB)를 모두 스트림에 상주시켰던 구조에서, 고정 상주 배열은 16KiB 수신 버퍼만 남았다. 이는 배열 배치에 대한 정적 설명이며 전체 연결 메모리·RSS·최대 메모리가 그만큼 줄었다는 실측 결과가 아니다. 메타데이터·실제 헤더·Connection·트레일러 할당과 할당자 비용이 추가된다. 헤더 준비에는 약 24.25KiB의 지역 배열, 트레일러 처리에는 지역 작업 공간이 필요하며 할당/복사 비용도 늘어난다. 향후 작업자 스택과 상태 머신 설계에서 다시 검토한다.

## 6. 공통 I/O와 테스트 코드 분리

src/socket_io.c는 기존 parse_port/open_listenfd 및 기한·취소·부분 I/O를 다루는 receive_with_deadline/send_all_with_deadline을 담는다. 프록시나 라우터에 의존하지 않는다.

tests/backends/server.c는 별도 main이다. src/socket_io.c와 src/http.c만 링크하며 프록시 코드·로컬 handler·CGI를 링크하지 않는다. 역할별 서버 ID와 고정 조회 데이터, 제출 echo, 테스트 전용 장애 모드만 제공한다. Python은 검증 스크립트에서 프로세스 실행·요청·결과 비교를 담당한다.

socket/connect/poll/getsockopt/recv/send/close/clock_gettime은 POSIX API, malloc/calloc/free는 표준 C 함수다. proxy_*와 *_with_deadline, open_listenfd는 직접 구현한 프로젝트 함수다. TCP는 바이트 전송을 제공하고 HTTP 경계·상한·타임아웃·정리는 서버가 책임진다.

## 한계와 다음 단계

- 현재 순차 처리다. A/B 분배가 병렬 처리나 처리량 향상을 보장하지 않는다.
- Iteration 5에서는 여러 작업자 + 작업자별 non-blocking 이벤트 루프를 구현하며 현재 blocking provider 계약을 재검토한다.
- 프로세스/스레드, 수락/분배, 공유 상태, 버퍼 상한·역압력, 요청 수/응답 시간/부하 기반 알고리즘은 사용자 선택 전이다.
- 연결 재사용, DNS, 클라이언트 HTTP/1.0 요청, 요청 수신 중 전달, 전체 응답 저장 등은 후속 검토에만 남겼다.
- 검증 후보는 iteration-4-tests.md에 있다. 컴파일만으로 동작·누수 검사를 통과했다고 판단하지 않는다. 현재 실행 결과는 iteration-4-routing-verification-results.md와 이전 단계 보고서에 구분해 기록했다.
## P2 수명 검증의 진입점

tests/unit/test_proxy_protocol.c는 C 구현 파일을 include하지 않고 proxy_internal.h를 통해 실제 모듈과 링크한다. test_proxy_lifecycle.c는 C/POSIX 자식 백엔드로 proxy_prepare_response를 실행하며 준비 실패 500, 헤더 해제 시점, 다중 Connection 보존, 트레일러 할당 실패/상한과 FD 회수를 확인한다. 할당 주입과 가변 전역 상태는 테스트 실행 파일에만 있으며 제품에는 추가하지 않았다. make verify는 기존 회귀와 엣지 40개를 실행한다.

## P2-9 A안: 단일 라우팅 테이블의 책임

router.h의 request_route는 함수 포인터와 목적지 역할만 담는다. proxy_route enum도 라우터가 판별하는 역할이므로 router.h에서 선언하고 proxy.h가 사용한다. 백엔드 IP·포트·선택 순번은 기존 proxy_config/proxy_selection_state에 그대로 있다.

router.c의 routes[]가 경로/메서드/정확 또는 접두사 일치/로컬 handler/프록시 역할을 함께 선언한다. 기존 최장 일치와 동일 길이의 정확 일치 우선 규칙은 유지했다. http_select_route는 이 테이블을 조회할 뿐 handler 실행·주소 선택·검증 콜백을 수행하지 않는다. 실패 결과는 LOCAL/NULL로 초기화해 이전 선택 결과가 재사용되지 않게 한다.

기존 proxy_match_request와 http_select_handler는 제거했다. test_http_parser의 로컬 응답 검증도 테스트 helper를 통해 같은 http_select_route를 사용한다. proxy_validate_request는 기존 보호 Connection 검사만 분리한 함수이며 경로나 메서드를 다시 판별하지 않는다. 동시성·IPv6·가변 백엔드·새 설정/정책 콜백은 추가하지 않았다.

## 성능 재측정 상태

기능·수명 검증 이후 사용자 승인 조건으로 보완 전후 성능48회를 완료했다. 비교 코드 흐름은 위와 같고 측정 중 제품 코드는 바꾸지 않았다. 결과는 iteration-4-performance-results.md에 있으며, 순차 구조나 소유권·기한을 성능 결과에 따라 자동 변경하지 않았다.
