# jungle-week-13-c-web-server

C/POSIX HTTP 서버다. 기존 Iteration 2 구현에 개인 Iteration 3 정적 파일 서빙을 추가했다.
팀 공통 범위는 docs/DEVELOPMENT_CONTEXT.md와 해당 Issue, 개인 선택은 PLAN.md를 참고한다.

## 실행

Ubuntu WSL2 셸:

```sh
make
./build/server 8080
# 다른 디렉터리를 파일 루트로 지정할 수도 있다.
./build/server 8080 /absolute/path/to/documents
```

서버는 시작 로그 없이 연결을 기다린다. 다른 터미널에서 수동 확인할 수 있다.

```sh
curl --noproxy '*' -i http://127.0.0.1:8080/hello
curl --noproxy '*' -i --data-binary 'hello' http://127.0.0.1:8080/echo
```

GET /와 /hello는 text/plain 및 줄바꿈 없는 Hello 5바이트다. POST /echo는 입력 바이트 그대로, application/octet-stream과 실제 본문 길이로 응답한다. 종료는 서버 터미널의 Ctrl+C다. 이 명령들은 사용 안내이며 이 작업에서 서버·curl을 실행했다는 뜻은 아니다.

## 정적 파일 사용과 코드 읽기

루트를 생략하면 현재 디렉터리의 public을 사용한다. 서버를 실행한 뒤 브라우저에서
`http://localhost:8080/static/`을 열면 HTML, CSS, SVG 샘플을 볼 수 있다.
기존 `/`는 Hello를 유지한다. `/static/`만 루트의 index.html로 연결하며 하위 디렉터리
목록이나 자동 index 탐색은 제공하지 않는다.

```sh
curl --noproxy '*' -i http://127.0.0.1:8080/static/
curl --noproxy '*' -i http://127.0.0.1:8080/static/css/style.css
curl --noproxy '*' -i http://127.0.0.1:8080/static/images/jungle.svg
curl --noproxy '*' -i http://127.0.0.1:8080/static/missing.html  # 404
curl --noproxy '*' -i http://127.0.0.1:8080/static/images/       # 403
curl --noproxy '*' --path-as-is -i http://127.0.0.1:8080/static/../secret  # 403
```

```text
main: open_static_root → 루트 FD 보관 → 연결마다 빌려주기
  → handle_client
      → 헤더 수신 / http_parse_request
      → http_route_request: /static/ GET 판정
      → prepare_static_file: 한 번 디코딩 → 경로 요소 검사 → openat → fstat → MIME
      → 기존 요청 본문 수신 / 필요한 100 Continue
      → send_response_header → send_file_body → 기존 send_bytes
      → finish_response → 파일 FD close / 요청 본문 free
  → 연결 FD close → 서버 종료 시 루트 FD close
```

파일은 16KiB씩 읽고 실제 읽은 길이만 전송한다. 64KiB 요청 본문 상한을 파일 응답에
적용하지 않는다. 정적 파일은 요청마다 열고, 처음 확인한 파일 크기를 Content-Length로
보낸다. 원본 파일이 전송 중 수정되는 경우 전체 내용의 스냅샷은 보장하지 않는다.

경로는 원본 slice를 바꾸지 않고 한 번 디코딩한다. +는 파일명 그대로이며 NUL/제어
문자/역슬래시/빈 요소는 400, .과 .. 요소 및 심볼릭 링크(중간 포함)·디렉터리·특수 파일·
권한 거부는 403, 없는 경로는 404다. 파일 루트 설정은 관리자가 선택한 경로이며,
클라이언트가 요청하는 하위 경로의 심볼릭 링크는 따라가지 않는다.

HTML/CSS/JS/PNG/JPEG/GIF/SVG 확장자를 MIME으로 매핑하며, 모르는 확장자는
application/octet-stream이다. 텍스트 샘플은 UTF-8이다. 오류 응답에는 내부 파일 경로를
노출하지 않는다. 파일 열기 등 서버 내부 오류는 500이며, 200 헤더 전송 뒤 파일 읽기가
실패하면 새 오류 응답을 붙이지 않고 연결을 닫는다.

추가 함수와 소유권을 순서대로 설명한 문서는
[Iteration 3 코드 해설](docs/iteration-3/jaeseong/iteration-3-walkthrough.md)을 참고한다.
설계 선택과 검증 상태는 해당 폴더의 요구사항·테스트 문서에 기록한다.

## 이번 지원 범위

- IPv4 0.0.0.0, 블로킹 순차 처리, 연결당 요청 하나, Connection: close.
- HTTP/1.1 origin-form만 지원하는 제한 구현이다. 완전한 HTTP/1.1 구현이 아니다. absolute-form은 거부하고 chunked/Transfer-Encoding 요청도 거부한다.
- 첫 물음표 앞 원래 경로로 라우팅한다. 파서는 percent escape 구문만 검사한다.
  /static/ 아래 파일 처리에서만 상대 경로를 한 번 디코딩하고 루트 경계를 검사한다.
- 헤더 이름 대소문자·OWS 처리, 유효한 단일 Host, 엄격한 Content-Length, Connection·Expect를 해석한다. 유효한 일반 미지원 필드는 의미 처리 없이 건너뛴다.
- 길이 정보 없음은 본문 0. 같은 값의 중복 길이·쉼표 목록·충돌 정보도 거절한다. 바이너리 본문 경계는 선언 길이로 정하며 내용으로 추측하지 않는다.
- 요청 타입 반영, JSON 파싱·압축 해제, keep-alive, 다음 요청 처리는 제외한다.
- HEAD는 지원하지 않아 501로 거부하되 HEAD 응답에는 본문을 보내지 않는다. 이를 HEAD 기능 지원으로 표시하지 않는다.

## 임시 상수와 소유권

| 위치 | 상수 | 임시값 |
|---|---|---:|
| src/http.h | REQ_HEADER_MAX_BYTES | 요청 줄·헤더·CRLFCRLF 합계 8192바이트 |
| src/http.h | REQ_BODY_MAX_BYTES | 본문 65,536바이트 |
| src/server.h | LISTEN_BACKLOG | 128 |
| src/server.h | IO_CALL_TIMEOUT_SECONDS | 수신·송신 호출별 5초 |
| src/server.h | HEADER_DEADLINE_SECONDS | accept 이후 헤더 전체 7초 |
| src/server.h | BODY_DEADLINE_SECONDS | 헤더 완료 이후 본문 전체 15초 |
| src/server.h | DRAIN_DEADLINE_SECONDS | 정상 응답 후 폐기 전체 1초 |
| src/server.h | DRAIN_MAX_BYTES | 응답 뒤 추가 수신 폐기 65,536바이트 |

숫자는 표준·최적값이 아닌 초기 정책이다. 본문은 유효 길이만큼 한 번 할당하고, 0이면 할당하지 않는다. 연결 처리 함수가 본문과 헤더를 소유하며 하위 함수는 빌려 사용한다. FD 최종 close는 main, 본문 free는 handle_client의 cleanup 한 곳이다.

수신은 호출별 제한과 전체 기한의 남은 시간 중 짧은 쪽을 사용한다. 전체 기한은 조금씩 수신하거나 EINTR가 발생해도 갱신하지 않는다. 전체 응답 송신 완료 기한은 별도로 도입하지 않았다.

응답 송신 후 shutdown(SHUT_WR), 제한적 폐기, close 순서다. 순차 서버는 폐기 대기 중 다음 연결을 처리하지 못하며, 상한에 도달해 닫는 경우 응답 전달을 보장하지 않는다. 서버 종료 신호 때에는 폐기 완료나 진행 중 응답 완료를 기다리지 않는다. accept의 신호 경합으로 무기한 대기가 남지 않도록 리스닝 소켓에도 호출별 수신 제한을 설정했다.

## 검증 상태와 명령

3단계에서 사용자가 확정한 실행 범위는 HTML/CSS/SVG/PNG 정상 응답과 MIME·길이·바이트
일치, 읽기 권한 거부 403, 없는 파일 404뿐이다. 해당 범위만 실행하는 명령은 다음과 같다.

```sh
make verify-static
```

현재 3단계 소스의 기능 검증은 실행 전이다. Ubuntu WSL의 디스크 파일 오류가 확인됐고,
대체 Docker 환경 사용 여부는 답변 대기다. 아래 컴파일 기록은 기존 2단계 이력이며
새 정적 파일 코드의 빌드·검증 성공을 뜻하지 않는다.

- 서버: 현재 Makefile의 -std=c11 -Wall -Wextra -Werror -O2로 컴파일 성공.
- 네 개 단위 테스트 바이너리: -fsanitize=address,undefined로 컴파일 성공. 실행하지 않았으므로 sanitizer 검사 통과를 의미하지 않는다.
- 두 E2E 스크립트: Python 구문 컴파일 성공. 실행하지 않았다.
- 공식 검증·성능 측정·비교 실험: 사용자 범위 확정/허락 전이며 미실행이다.

허락 후 사용할 후보 명령:

```sh
make test
make verify
```

make test는 기존 세 단위 테스트와 test_http_parser, test_static_file을 실행한다.
make verify는 1·2·3단계 E2E를 실행한다. test_static_file의 임시 파일은 /tmp에 만들며,
3단계 E2E는 임시 루트에서 실제 권한 거부와 바이너리 무결성을 확인한다.
검증기 실행자가 root이면 E2E 서버는 비루트 UID로 실행한다.
SERVER_BIN 환경 변수로 별도 빌드의 실행 파일을 지정할 수 있다.
기본 E2E에 기존 시간 기한·폐기 상한·할당 실패 주입의 모든 분기가 포함되지는 않는다.
광범위한 조합·퍼징·장시간/경합 회귀 및 성능 계측·부하는 별도 범위다.

## 설명과 기록

- docs/iteration-2/Snoopykid/iteration-2-walkthrough.md: main부터 대화의 결정과 연결한 코드 흐름.
- docs/iteration-2/Snoopykid/iteration-2-requirements.md: 최소 범위·표준·선택 기록.
- docs/iteration-2/Snoopykid/iteration-2-tests.md: 기본 성공·실패 후보와 아직 남은 검증.
- EXPERIMENTS.md: 후속 비교·chunked·URI 설계 검토 목록.
- 이전 개인 구현과 build/legacy/의 보관 바이너리는 현재 Makefile의 빌드 대상이 아니다.

## 빌드 산출물과 라우터 분리
make는 build/server 및 build/obj/*.o를 만든다. 테스트 바이너리도 build/test_*에 생성한다. make test_parse_port 등 기존 개별 빌드 명령은 새 경로 대상의 별칭이다. make clean은 현재 서버·객체·테스트 산출물을 지우며 build/legacy의 보관 파일은 유지한다. build 전체는 Git 추적에서 제외한다.

src/http.c는 구문 해석과 상태 문구, src/router.c는 경로/메서드 판별과 응답 내용 선택, src/server.c는 I/O와 연결 자원 수명을 담당한다. 라우터의 응답 본문은 기존 공간을 빌려 참조한다. 기존 HTTP 동작과 컴파일 옵션을 유지했다.
