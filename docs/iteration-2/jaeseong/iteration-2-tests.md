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

### 2026-11-20: Linux Docker 검증

Windows 호스트의 Ubuntu WSL은 배포판 VHD 파일을 찾지 못해 실행되지 않았다.
기존 Ubuntu 계열 개발 Docker 이미지에 프로젝트를 마운트하고 GCC/make/Python 3/curl로 실행했다.

| 명령 | 결과 |
| --- | --- |
| `make all test` | GCC 경고를 오류로 취급한 빌드 성공, ASan/UBSan 단위 실행 파일 5종 통과 |
| `make verify` | 실제 서버 E2E 17개 통과 (11.467초) |
| `make verify-iter1` | 이전 단계 회귀 통과, 특권 포트 검사는 환경 조건에 따라 건너뜀 |
| `make sanitize` | 서버까지 ASan/UBSan으로 재빌드, 단위 5종 및 E2E 17개 통과 (E2E 11.813초) |
| `git diff --check` | 공백 오류 없음 |

E2E에는 실제 curl GET/POST, 1 MiB 본문, 빈 본문, NUL/비UTF-8/CRLF 본문,
분할 헤더·본문, 미완성 요청 EOF, 헤더/본문 타임아웃, 길이·경로 상한,
오류 후 복구, 반복 연결, peer reset, 포트 충돌, SIGINT/SIGTERM 종료가 포함됐다.
각 응답은 Content-Length와 실제 본문 바이트 수, Connection: close를 함께 검증한다.

기존 E2E의 두 가정도 보완했다. HTTP/1.1 요청에 Host를 넣고 한 recv에 응답이
모두 온다는 가정을 제거했다. 헤더 상한 오류는 새 정책에 맞춰 431을 기대한다.
비루트 UID라도 컨테이너 설정상 80번 포트를 사용할 수 있으므로 실제 bind 권한을
확인한 경우에만 특권 포트 실패를 검증한다. 해당 환경에서는 이 항목을 건너뛰었다.

macOS에서 직접 실행한 결과 및 장기 부하/누수 측정은 이번 결과에 포함하지 않는다.
