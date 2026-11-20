# Iteration 2: 요청 파싱, GET/POST, 동적 응답

## 범위와 기준

기존 `main → accept → handle_client → close` 흐름과 `GET /`의 `Hello` 응답을 유지한다.
로컬 기준은 AGENTS.md, DEVELOPMENT_CONTEXT.md와 사용자가 요청한 Iteration 2 범위다.
Issue #2는 연결 도구/CLI 인증 및 공개 페이지 조회 실패로 내용을 확인하지 못했다.
따라서 이 문서는 이슈와 대조 완료된 통합 기준이 아닌 개인 구현의 경계를 기록한다.

## In-Scope

- 헤더 8 KiB, 요청 대상 2048바이트, 본문 1 MiB 상한.
- HTTP/1.0, HTTP/1.1 요청 라인 및 Host/Content-Length/Connection 해석.
- HTTP/1.1의 Host 필수, 중복 Host/Content-Length 및 모호한 길이 정보 거부.
- Content-Length 기반 본문 수신. 함께 도착한 본문 보존, 부족한 길이만 추가 recv.
- GET /: 기존 Hello, GET /hello?name=Jungle: Hello, Jungle! 개행.
- name은 최대 256바이트, %HH와 + 디코딩. 제어 문자, 잘못된 이스케이프 거부.
- POST /echo: 본문을 바이너리 그대로 응답. POST의 Content-Length 누락은 411.
- 오류: 400/404/405/408/411/413/414/417/431/500/501/505.
- 헤더/본문 각각 길이 기반 처리, 모든 응답 Connection: close.
- 요청별 할당 본문은 handle_client가 free, connfd는 main이 close.
- recv/send 5초 호출별 타임아웃. 신호 중단을 상위로 전달해 종료 루프 복귀.

## Out-of-Scope

정적 파일, Reverse Proxy, 동시성, keep-alive, pipelining, chunked 디코딩,
JSON/form 파서, CGI 실행, 요청 전체 deadline은 후속 단계다.
Transfer-Encoding은 지원하지 않음을 응답하고, Expect는 417로 거부한다.
헤더 이후 추가 요청 바이트는 재사용하지 않고 연결을 닫는다.

## 설계 흐름

main → handle_client → 헤더 수신 → parse_http_request → 본문 수신
→ route_request → send_response → 본문 free → main에서 connfd close.

server.c는 소켓 I/O, http.c는 바이트 해석과 응답 결정에 집중한다.
응답 본문은 요청 버퍼 또는 응답 구조체 내부 메모리를 빌려 쓰며 전송 후에 해제한다.
