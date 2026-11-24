# Iteration 4 실제 코드 흐름

기록일: 2026-11-24. 구현과 컴파일·링크 확인까지 완료했으며 공식 검증은 실행하지 않았다. 이전의 설정·선택 모듈 단독 단계 이후 main과 중계 흐름을 통합했다.

## 1. main: 서버 수명과 변경 가능한 상태

src/main.c의 main은 실행 인자 <port> <proxy.conf>를 확인하고 parse_port → proxy_load_config → public 디렉터리 확인 → setup_signals → open_listenfd 순서로 시작한다.

설정은 struct proxy_config, 조회 순번은 struct proxy_selection_state로 main의 지역 공간에 둔다. 종료 신호 표시 외에 새 변경 가능한 전역 상태는 없다. accept로 얻은 client_fd는 main이 소유하고 handle_client에 빌려 준다. handle_client가 돌아오면 main이 한 번 close한다.

설정 파일은 사용자가 이동한 build/proxy.conf.example을 실행 인자로 지정한다. build/ 무시 규칙과 clean의 명시적 산출물 목록 때문에 Git에 올라가지 않고 clean으로도 삭제하지 않는다.

## 2. handle_client: 요청 하나의 조율

src/server.c는 handle_client를 파일 위에 두었다.

1. receive_request_headers: 기존 8KiB 버퍼와 헤더 전체 7초 기한으로 요청 헤더와 함께 도착한 본문 일부를 보존한다.
2. http_parse_request: 기존 HTTP/1.1·origin-form·Host·Content-Length·64KiB 상한·미지원 정책을 검증한다. 일반 필드는 원본 헤더의 slice로 순회할 수 있다.
3. proxy_match_request: 제출/조회 경로를 먼저 판별한다. 그 외는 기존 http_select_handler로 보낸다. 경로/메서드가 잘못되면 본문을 받기 전에 기존 오류 흐름으로 간다.
4. request_body: 검증한 길이만큼 한 번 할당하고 동반 수신분을 복사한 뒤 부족분만 받는다. 필요한 100 Continue는 프록시가 보낸다. 불완전 본문은 백엔드로 전달하지 않는다.
5. proxy_select_backend: 제출은 제출 주소, 두 조회 경로는 공유하는 A/B 순번이다. 본문 수신까지 성공한 요청에서 실제 선택하고 선택 직후 순번을 넘긴다. 재전송하지 않는다.
6. 프록시는 proxy_prepare_response, 로컬은 기존 handler를 실행한다.
7. send_prepared_response가 전송과 provider cleanup을 수행한다. 성공하면 기존 제한적 수신 폐기를 하고 돌아온다.
8. cleanup에서 요청 본문을 해제한다. 클라이언트 FD는 닫지 않는다.

request_header, request_body, header_end_offset, body_received_bytes, request_io, client_response_io처럼 역할과 바이트 수를 드러내는 이름으로 정리했다.

## 3. proxy_prepare_response: 백엔드 수명 시작

src/proxy_transfer.c의 상위 함수는 아래 순서다.

- calloc으로 응답 상태 하나를 만든다. backend_fd 초기값은 -1이다.
- connect_backend: socket → close-on-exec → 일시적 non-blocking connect → poll/SO_ERROR → blocking 복원. 전체 1초이며 서버 동시성 모델을 바꾸는 것이 아니다.
- forward_request_to_backend: 원래 메서드·target을 보존하고 Host·Content-Length·Connection·Via를 구성한다. 일반 헤더는 보존하고 hop-by-hop 및 지명 헤더는 제거한다. Expect는 재전달하지 않는다. 완성된 요청 본문을 보내며 전체 5초다.
- receive_backend_response_headers: 전체 1초 안에서 HTTP/1.1 상태·필드·길이·프레이밍을 검사한다. 100은 중복 전달하지 않고 다른 일반 1xx는 출력 헤더 공간에 보관한다. HTTP/1.0·101·잘못된 응답은 거절한다.
- build_client_response_headers: 원래 상태 문구와 전달 대상 헤더를 보존하면서 연결별 필드 제거, 프레이밍 재생성, Via 추가를 수행한다. Content-Type을 임의로 만들어 덮어쓰지 않는다. Set-Cookie 같은 반복 필드를 합치지 않는다.
- body_deadline을 시작하고 response에 헤더 slice와 body_provider를 전달한다.

중간 실패는 상태 소유 FD·메모리를 정리한 뒤 local 500/502/504 응답을 만든다. 성공 후에는 소유권이 provider cleanup 경로로 넘어간다.

## 4. read_backend_body_chunk: 다음 조각만 제공

한 개의 16KiB 버퍼를 재사용한다. 소켓 read 대기 중 다른 연결을 처리하는 구조는 아직 아니다.

- Content-Length: 남은 바이트 수를 줄이고 초과분은 현재 본문에 넣지 않는다. 조기 EOF는 실패다.
- chunked: 크기·확장·데이터 뒤 CRLF·0 청크·트레일러를 검증하고 데이터 조각만 전송기에 빌려 준다.
- 연결 종료형: 정상 EOF까지 읽는다. 정상 FIN만으로 백엔드가 원래 의도한 본문 전체를 보냈는지 알 수는 없다.
- 본문 없음 상태는 데이터를 만들지 않는다.
- 무진행 5초·전체 본문 수신 15초를 적용한다. 마지막 클라이언트 바이트 전달까지의 전체 기한을 보장하는 정책과는 다르다.

헤더·청크 메타데이터에는 기존 8KiB 상한을 재사용한다. 스트림 버퍼 16KiB와 응답 상태 전체 메모리는 다르다. 별도 헤더·트레일러 저장 공간이 있다.

## 5. send_prepared_response: 최종 전송과 한 번 정리

로컬 응답은 기존 send_response를 사용한다. 프록시 응답은 검증된 wire_header를 먼저 보낸다.

그 다음 고정 길이는 send_body, chunked/연결 종료형은 send_chunked_body로 보낸다. 청크 종료와 검증된 트레일러는 provider가 정상 완료했을 때만 보낸다. 공급자 실패·송신 실패에는 종료 청크나 새 HTTP 오류를 붙이지 않는다.

성공·오류 모두 provider cleanup을 정확히 한 번 호출한다. cleanup_backend_response는 백엔드 FD를 닫고 응답 상태를 해제한다. 요청 본문은 handle_client가 그 이후 해제한다.

## 6. 공통 I/O와 테스트 코드 분리

src/socket_io.c는 기존 parse_port/open_listenfd 및 기한·취소·부분 I/O를 다루는 receive_with_deadline/send_all_with_deadline을 담는다. 프록시나 라우터에 의존하지 않는다.

tests/backends/server.c는 별도 main이다. src/socket_io.c와 src/http.c만 링크하며 프록시 코드·로컬 handler·CGI를 링크하지 않는다. 역할별 서버 ID와 고정 조회 데이터, 제출 echo, 테스트 전용 장애 모드만 제공한다. Python은 검증 스크립트에서 프로세스 실행·요청·결과 비교를 담당한다.

socket/connect/poll/getsockopt/recv/send/close/clock_gettime은 POSIX API다. proxy_*와 *_with_deadline, open_listenfd는 직접 구현한 프로젝트 함수다. TCP는 바이트 전송을 제공하고 HTTP 경계·상한·타임아웃·정리는 서버가 책임진다.

## 한계와 다음 단계

- 현재 순차 처리다. A/B 분배가 병렬 처리나 처리량 향상을 보장하지 않는다.
- Iteration 5에서는 여러 작업자 + 작업자별 non-blocking 이벤트 루프를 구현하며 현재 blocking provider 계약을 재검토한다.
- 프로세스/스레드, 수락/분배, 공유 상태, 버퍼 상한·역압력, 요청 수/응답 시간/부하 기반 알고리즘은 사용자 선택 전이다.
- 연결 재사용, DNS, HTTP/1.0, 요청 수신 중 전달, 전체 응답 저장 등은 후속 검토에만 남겼다.
- 검증 후보는 iteration-4-tests.md에 있다. 컴파일은 동작·누수 검사 통과의 근거가 아니다.