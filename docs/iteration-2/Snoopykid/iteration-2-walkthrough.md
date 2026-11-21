# Iteration 2: main()에서 시작하는 코드 흐름

상태: 최소 범위 구현과 컴파일 확인을 마쳤다. 아래는 실제 코드 흐름 설명이며 동작 테스트 통과 보고가 아니다. 공식 검증·성능 측정은 아직 실행하지 않았다.

## 1. main은 서버의 시작과 FD 수명을 맡는다

src/main.c의 main은 parse_port → setup_signals → open_listenfd → accept 반복 순서로 진행한다. parse_port·setup_signals·open_listenfd·handle_client는 직접 만든 함수이고, socket·bind·listen·accept·recv·send·close·sigaction은 운영체제 인터페이스다.

parse_port는 필수 포트 인자가 숫자이고 1~65535인지 검사한다. setup_signals는 SIGPIPE를 무시하고 SIGINT/SIGTERM에는 종료 표시만 바꾸는 핸들러를 연결한다. 신호 핸들러 안에서 메모리나 FD를 정리하지 않는 것은 함께 선택한 정책이다.

open_listenfd는 socket → SO_REUSEADDR 및 리스너 수신 제한 → bind(IPv4 0.0.0.0) → listen을 수행한다. 이 FD는 연결을 받는 용도다. accept가 반환한 연결 FD로 실제 HTTP 데이터를 주고받는다. CSAPP 11장의 listening descriptor와 connected descriptor의 구분이다.

main은 accept 직후 CLOCK_MONOTONIC으로 시각을 기록하고 handle_client에 연결 FD·종료 표시의 주소·수락 시각을 빌려준다. handle_client가 끝나면 main이 연결 FD를 한 번 닫는다. 프로그램이 종료되면 리스너도 main이 닫는다. 연결 상태를 전역에 두지 않는 선택에 따라 전역에는 프로세스 종료 표시만 있다.

## 2. handle_client가 한 요청의 자원을 소유한다

src/server.c의 handle_client는 8KiB 헤더 배열, 파싱 결과 http_request, 본문 포인터와 현재 수신 길이를 지역 변수로 가진다. 처음 본문 포인터는 NULL이다.

본문은 형식·오버플로우·64KiB 상한 검증을 통과한 길이만큼 malloc 한 번을 한다. 길이 0은 할당하지 않는다. 하위 함수들은 공간을 빌려 쓰고, 함수 마지막 cleanup의 free(body)에서 소유자가 정리한다. 정상·EOF·시간 초과·송신 실패·종료 경로가 같은 정리 지점을 사용한다.

http_slice는 표준 API가 아니라 이번 코드의 포인터+길이 자료형이다. 메서드·요청 대상·버전·Host·경로는 원본 헤더를 가리킨다. 파서가 헤더를 NUL로 쪼개거나 별도로 문자열 복사하지 않으며, 참조는 해당 연결 처리 중에만 사용한다. 본문은 별도 소유 공간의 바이트 배열이다.

## 3. 헤더 끝과 수신 전체 길이는 다르다

receive_header는 CRLFCRLF를 찾을 때까지 여러 번 recv할 수 있다. 경계가 recv 사이에 나뉠 수 있어서 앞 수신의 마지막 3바이트와 이어서 검사한다.

- received: 한 번 이상 recv로 실제 받은 전체 길이.
- header_end: CRLFCRLF를 포함한 헤더 끝, 즉 본문의 시작 위치.
- received - header_end: 헤더와 함께 이미 도착한 데이터. 이 중 선언 본문 길이까지만 본문으로 사용한다.

기존 recv_until_header_end는 같은 저수준 수신 로직을 쓰되 out_len이 전체 수신 길이라는 1단계 테스트 계약을 유지한다. 새 연결 조율 흐름은 별도로 header_end까지 받는다.

예를 들어 Content-Length: 5인 요청의 헤더와 ab가 같이 왔다면 5바이트를 할당하고 ab를 보존한다. 추가 recv에는 부족한 3바이트만 요청한다. 다음 입력이 cde와 다른 요청을 함께 담더라도 현재 본문은 abcde까지만이며, 나머지는 두 번째 요청으로 실행하지 않는다.

## 4. 순수 파서가 입력의 의미와 안전성을 검사한다

src/http.c의 http_parse_request는 소켓에서 읽지 않고 받은 헤더 범위만 검사한다. 잘못된 길이를 메모리 할당 크기로 사용하지 않도록 이 검사를 할당보다 먼저 수행한다.

- HTTP/1.1·origin-form 제한. 다른 정상 버전은 505, 정상 absolute-form은 501로 거부하는 제한 구현이다.
- URI의 잘못된 percent 구문·공백·fragment·제어문자는 거부한다. 유효한 escape도 디코딩하지 않고, 라우팅은 첫 물음표 앞 원래 경로로 한다. 정적 파일 전에는 이 결정을 루트 밖 접근 방지와 함께 재검토한다.
- Host는 유효한 단일 값이어야 한다. 이름 대소문자와 값의 OWS를 처리하되 잘못된 필드 이름·obs-fold·NUL·제어문자는 거부한다.
- 같은 값이라도 Content-Length 중복·쉼표 목록은 거부한다. 숫자 형식·정수 오버플로우와 64KiB 상한을 구분해 400 또는 413을 정한다.
- Content-Length/Transfer-Encoding 충돌은 400. 잘못된 전송 프레이밍은 400, 유효한 chunked도 이번에는 지원하지 않아 501이다. 완전한 HTTP/1.1 지원이라고 주장하지 않는다.
- 길이 정보가 없으면 본문 0이며, 소켓에 더 온 바이트를 보고 본문이라고 추측하지 않는다.

## 5. 본문을 요청하기 전에 경로와 Expect를 판단한다

http_route_request는 GET / 및 /hello, POST /echo를 구분한다. 이 함수도 I/O를 하지 않는다. 모르는 경로는 404, 지원하는 GET/POST와 해당 경로가 맞지 않으면 405와 Allow, 미구현 메서드는 501이다.

정상 Expect: 100-continue 요청의 본문이 더 필요하면 handle_client가 HTTP/1.1 100 Continue와 빈 줄을 먼저 보낸다. 클라이언트는 이를 보고 본문을 보낼 수 있다. 헤더만으로 413 등 오류를 알면 본문을 받기 전에 최종 오류를 보낸다. 본문이 이미 전부 도착했다면 100을 생략한다.

TCP에서는 클라이언트의 send 한 번이 서버의 recv 한 번을 보장하지 않는다. 따라서 클라이언트가 헤더·본문을 한 번에 보냈다는 사실만으로 100 생략 경로가 실행됐다고 테스트에서 단정하지 않는다. 기존 E2E도 이 점을 고려해 중간 응답과 최종 응답을 분리해서 읽는다.

## 6. 수신 기한은 매번 새로 시작하지 않는다

현재 임시값은 호출별 5초, accept 이후 헤더 전체 7초, 헤더 완료 이후 본문 전체 15초다. CLOCK_MONOTONIC을 사용하므로 벽시계 변경을 전체 기한 계산에 쓰지 않는다.

set_call_timeout은 호출별 5초와 해당 전체 기한의 남은 시간 중 짧은 값을 사용한다. 조금씩 받아도 전체 기한은 늘리지 않는다. 무입력은 5초 호출 제한으로 먼저 끝날 수 있고, 계속 조금씩 보내 호출 제한을 피하더라도 전체 기한은 남는다.

EOF는 시간 초과와 다르다. 선언 본문보다 부족한 EOF는 가능한 경우 400, 수신 시간 초과는 408이다. 부족분을 채워 넣거나 정상 echo하지 않는다. 종료 표시가 있으면 응답·폐기 대기보다 자원 정리를 우선한다. 전체 응답 송신 완료 기한은 이번에 도입하지 않았다.

## 7. 응답 생성과 부분 송신

GET / 및 /hello는 text/plain과 줄바꿈 없는 Hello 5바이트다. POST /echo는 실제 완성된 본문 바이트를 application/octet-stream으로 반환하고 그 바이트 수를 Content-Length로 쓴다. 요청의 Content-Type·JSON 의미·압축 해제를 응답 처리에 끌어오지 않는 것은 함께 정한 최소 범위다.

send_response는 상태·타입·길이·Connection: close와 필요한 Allow를 만든다. send_bytes는 send가 실제 돌려준 길이만큼 진행하며 남은 바이트를 반복 송신한다. strlen은 고정 상태 문구 등 신뢰된 코드 문자열에만 쓰고, 요청 본문 길이는 명시적 바이트 수를 사용한다.

정상 GET/POST가 아닌 HEAD는 501로 거부하되 HTTP 응답 본문을 붙이지 않는다. 이는 HEAD 엔드포인트 지원을 추가한 것이 아니다.

## 8. 응답 뒤 폐기와 최종 정리

finish_response는 응답 송신이 끝난 뒤 shutdown(SHUT_WR)로 쓰기 방향만 닫는다. 남은 입력을 최대 1초 또는 64KiB까지만 보관 없이 읽어 버리고 끝낸다. 추가 데이터를 본문이나 다음 요청으로 처리하지 않는다. main이 그다음 FD를 닫는다.

이는 읽지 않은 데이터를 둔 즉시 close의 TCP reset 위험을 줄이기 위한 선택이다. 상한에 도달한 뒤 종료하면 응답 전달이 완전히 보장되지는 않는다. 순차 처리에서는 이 대기 동안 다음 연결을 처리하지 못하는 비용도 있다. 1초·64KiB는 임시값이다. SIGINT/SIGTERM 때는 이 폐기를 기다리지 않고 빠져나와 본문 free와 FD close로 이어진다.

## 지금 확인한 것과 아직 확인하지 않은 것

확인한 것은 C11·경고 오류 처리·현재 -O2로 서버가 컴파일되고, sanitizer 옵션으로 네 단위 테스트 바이너리가 컴파일되며, 두 Python 스크립트의 구문 컴파일이 성공했다는 사실이다.

서버·curl·테스트 바이너리·E2E는 실행하지 않았다. 따라서 실제 응답·메모리/FD 안전·타임아웃·종료·성능이 검증됐다고 주장하지 않는다. 현재 코드 흐름 설명과 기본 후보·실행 범위를 검토한 뒤 공식 검증을 진행한다. 광범위한 조합·장시간·경합 회귀는 6단계에서 확대한다.

## 라우터 분리 후 실제 함수 연결 (2026-11-21)
현재 실행 파일은 ./build/server다. 기존 설명의 응답 선택 책임은 이제 src/router.c로 이동했다.

main → accept → handle_client → receive_header → http_parse_request(src/http.c) → http_route_request(src/router.c) → 본문 malloc/동반 데이터 복사/receive_some → router_prepare_response(src/router.c) → send_response → send_bytes → finish_response → free → main의 close 순서다.

http_route_request는 본문을 받기 전에 경로·메서드를 검사하고 route/Allow 또는 오류 상태를 돌려준다. router_prepare_response는 응답 시점에 상태·Content-Type·본문 포인터·길이·Allow를 담은 router_response를 반환한다. Hello는 상수 5바이트, echo는 수신 본문을 그대로 참조하고, 오류는 http_reason의 기존 문구를 참조한다. 메모리 소유권·소켓 I/O는 라우터로 이동하지 않았다. send_response는 이 결과를 HTTP 헤더와 본문으로 송신한다.