# Iteration 3: main부터 파일·동적 응답까지

상태: 구현 및 컴파일 확인 단계. 아래 흐름은 동작 테스트 통과 증거가 아니다.

## 1. main의 시작과 루트 FD
main(argc, argv)는 인자 세 개(프로그램명 포함)를 요구한다. parse_port(argv[1], &port), setup_signals() 후 static_open_root(argv[2])를 호출한다. static_open_root는 POSIX open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)을 호출해 디렉터리 FD를 반환한다. CLI의 루트 링크는 OS가 해석한다. 열기 실패/일반 파일은 오류 출력 후 시작 중단이다.

main은 루트를 연 후 open_listenfd(port), accept를 실행한다. handle_client(connfd, &g_running, &accepted_at, rootfd)에 루트 FD를 빌려준다. 연결 close와 루트 close는 main이 소유한다.

## 2. handle_client의 공통 요청 처리
receive_header → receive_some → recv로 헤더를 모은다. http_parse_request는 기존 HTTP/1.1 origin-form·Host·길이 규칙을 유지하고, http_route_request는 본문 수신 전에 메서드/경로를 결정한다.

GET /static/...는 HTTP_ROUTE_STATIC, GET /greet는 HTTP_ROUTE_GREET다. 정적 경로와 greet에 POST면 파일 존재 검사 전에 405 + Allow: GET. 기존 GET /·/hello와 POST /echo는 그대로다. 미구현 메서드는 501이다. 기존 선언 길이 기반 본문 수신과 Expect/타임아웃 처리를 마친 뒤 응답을 준비한다.

## 3. static_open_file의 안전한 경로 매핑
handle_client → static_open_file(rootfd, request.path, &filefd, &length, &type) → static_decode_path 순서다. request.path는 원래 '?' 앞 부분만 가리키므로 쿼리는 파일명이 아니다.

static_decode_path는 원문 /static/을 제거하고 %HH를 한 번만 해석한다. +는 공백으로 바꾸지 않는다. 인코딩된 /, 역슬래시, NUL·바이트 제어문자(00~1F, 7F)는 400. .·.. 세그먼트는 403, 반복 슬래시는 400, 빈 경로나 끝 /는 403. 유효한 정적 파일명의 고위 바이트는 OS 파일명 바이트로 처리하며 greet의 UTF-8 규칙을 자동 적용하지 않는다.

static_open_file은 경로 전체 검사를 마친 뒤 요소를 하나씩 NUL로 나누어 openat(directory_fd, component, O_NOFOLLOW | ...)를 호출한다. 중간 디렉터리는 O_DIRECTORY, 마지막 요소도 O_NOFOLLOW다. FIFO 등이 open에서 서버를 대기시키지 않도록 O_NONBLOCK으로 연 뒤 fstat로 일반 파일만 허용한다. 이 플래그는 네트워크 동시성 모델 변경이 아니다.

중간 FD는 다음 요소를 열고 닫는다. 실패하면 보유한 중간 FD를 닫고 HTTP 상태를 반환한다. 마지막 일반 파일 FD만 handle_client로 소유권을 넘긴다. MIME은 디코딩된 마지막 파일명의 확장자로 정한다. 없음/중간 파일 404, 권한·링크·비정규 파일 403, OS 이름 길이 초과 414, 기타 500이다.

루트 문자열을 덧붙인 뒤 접두사를 검사하는 방식이 아니다. 다만 권한 있는 공격자가 디렉터리를 다른 곳으로 이동하거나 마운트·하드링크 등을 조작하는 상황까지 POSIX openat가 격리한다고 주장하지 않는다. 읽기 전용 서버/신뢰하는 게시자의 운영 계약이 필요하다.

## 4. send_response → send_file_body → read/send_bytes
응답 정보에는 body kind(MEMORY/FILE), uintmax_t 길이, 파일 FD가 있다. send_response는 최초 fstat 길이를 Content-Length로 포맷하고 헤더를 send_bytes로 보낸다. 파일 종류이면 send_file_body를 호출한다.

send_file_body는 지역 16KiB 배열을 반복 재사용한다. read(filefd, buffer, min(remaining, 16384))의 실제 반환 바이트를 send_bytes로 전송하고 remaining에서 뺀다. send_bytes는 기존 send의 부분 송신/EINTR/호출별 타임아웃 처리를 유지한다. 파일 전체 malloc은 없다.

파일이 커져도 최초 길이까지만 보낸다. 짧은 양의 read는 이어서 읽고, remaining이 남았는데 0이 오면 조기 EOF 오류다. 읽기 오류·클라이언트 송신 실패도 -1로 전파한다. 헤더를 이미 보냈으므로 500 응답을 덧붙이지 않고 로그 후 공통 정리로 간다. 파일 내용 스냅샷, 동일 크기 덮어쓰기 탐지, 디스크 I/O 전체 기한은 제공하지 않는다.

성공하면 기존 finish_response의 shutdown(SHUT_WR)·최대 1초/64KiB 폐기를 적용한다. 실패하면 이 성공 후 폐기를 건너뛴다. cleanup에서 파일 FD를 닫고 생성 본문/요청 본문을 해제한다. main이 연결 FD를 닫는다.

## 5. greet_prepare_response의 두 번 읽기·한 번 할당
handle_client → greet_prepare_response(request, &generated_body)다. 첫 스캔은 원래 쿼리를 &로 나눈 뒤 첫 =로 키·값을 나누고, next_byte로 각각 %HH를 한 번 해석하며 +를 공백으로 바꾼다. %26/%3D는 새 구분자가 아니다.

validate_component는 모든 키·값을 할당 없이 읽어 UTF-8·제어문자를 검사하고 디코딩 길이를 센다. 디코딩된 키가 정확히 소문자 name인지도 판별한다. na%6De는 name이고 name=A&na%6De=B는 중복 400이다. 유효한 모르는 키의 의미는 무시한다. name 누락은 World, 빈 name은 400이다.

현재 UTF-8 검사는 과잉 인코딩·대리 코드 포인트·U+10FFFF 초과·불완전 시퀀스를 거절한다. 금지 제어문자는 U+0000~001F/U+007F~009F다. 정규화·대소문자 변환·공백 제거는 하지 않는다. 이 범위의 확대 여부는 검증 체크포인트에서 재선택한다.

검사를 통과하면 decoded name byte count + 9를 한 번 malloc한다. "Hello, " 7바이트, name, "!\n" 2바이트를 채운다. 본문은 NUL 종단에 의존하지 않고 실제 바이트 길이를 사용한다. "Alice"는 총 14바이트다. 응답은 text/plain; charset=utf-8이다. handle_client가 넘겨받은 generated_body를 cleanup에서 해제한다.

## 6. API와 소유권 구분
직접 작성: static_open_root/static_open_file/static_decode_path/static_mime_type, http_route_request/router_prepare_response, greet_prepare_response/validate_component, send_response/send_file_body/send_bytes.
C/POSIX: open/openat/fstat/fstatat/read/send/close/malloc/free/rename.

| 자원 | 최종 소유·정리 |
| --- | --- |
| 리스너·루트·클라이언트 FD | main |
| 정적 파일 FD·요청/생성 본문 | handle_client |
| 중간 경로 디렉터리 FD | static_open_file |
| 헤더의 슬라이스 | handle_client의 헤더를 참조, 별도 free 없음 |