# Iteration 2 테스트 기준 (구현 전 정의)

## 순수 파서/라우팅 단위 테스트

- GET/POST, HTTP/1.0 및 1.1, Host/Connection/Content-Length 해석.
- 콜론 주변 공백, 헤더 이름 대소문자, OWS 제거.
- 잘못된 요청 라인/개행/NUL/헤더 이름/버전/Host 누락·중복.
- 음수/부호/문자/빈 Content-Length, 정수 오버플로우, 본문 크기 상한.
- 중복 길이, Transfer-Encoding+Content-Length, Expect 거부.
- 루트 기존 응답, 이름 URL 디코딩, 기본 이름, 잘못된 인코딩.
- 404/405와 Allow, 미지원 메서드 501, 바이너리 echo 및 정확한 길이.

## 소켓 및 E2E 테스트

- 기존 parse_port/recv_header/send_all 테스트를 회귀 실행.
- 헤더 경계 및 요청 라인을 나눠 보내도 같은 응답.
- 헤더+본문 동반 도착 및 본문을 여러 번 나눠 보내는 경우.
- 본문 완성 전 성공 응답 없음, NUL/CRLF/비 UTF-8 바이트 그대로 echo.
- 본문 조기 EOF/타임아웃, 최대 크기와 한 바이트 초과, 길이 누락.
- 오류 연결 후 정상 요청 처리, 반복 연결, 사용 중 포트, SIGINT/SIGTERM.
- 실제 curl GET/POST, ASan/UBSan 단위 테스트 및 E2E 실행.

## 실행 결과

구현과 검증 후 아래에 명령 및 결과를 추가한다.
