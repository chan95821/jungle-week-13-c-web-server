# jungle-week-13-c-web-server

> 현재 코드: Iteration 3 기반의 Iteration 4 Reverse Proxy 개인 초안.
> 아래 기존 Iteration 2 설명·경로·검증 기록은 과거 기준이며 현재 전체 구현 상태를 나타내지 않는다.
> 최신 흐름·제한·검증 상태는 [Iteration 4 코드 읽기](docs/iteration-4/jaeseong/iteration-4-walkthrough.md)를 우선 참고한다.

## 현재 실행 — Iteration 4

Ubuntu WSL2의 저장소 루트에서 `make` 후 실행한다. 테스트 백엔드는 각기 별도 터미널에서 실행한다.

```sh
python3 scripts/iteration4_backend.py --port 8001 --role submission --id submission
python3 scripts/iteration4_backend.py --port 8002 --role status --id A
python3 scripts/iteration4_backend.py --port 8003 --role status --id B
```

```sh
./build/server 8080 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003
```

- `POST /api/submissions` → 제출 서버. `GET /api/submission-status` → 조회 A/B 라운드로빈.
- 경로·query·본문 바이트 보존, 백엔드 무진행 제한 1초, 자동 재시도 없음.
- 기존 `/hello`, `/echo`, `/cafe`, `/menu`, `/assets/...`, `/greet`, `/cgi-bin/...` 유지.
- 요청은 기존 Content-Length·64KiB 상한으로 수신 완료 후 전달, 응답은 스트리밍.
- 기존 `./build/server 8080`은 프록시 없이 로컬 서빙만 한다.
- 정적 루트 `public`은 여전히 작업 디렉터리 기준 고정이다.
- C 컴파일·공식 검증은 미확인/보류, 성능 측정은 미실행. Ubuntu 디스크 부재와 컴파일 도구 부재로 이번 C 빌드 확인은 하지 못했다.
- `make test`, `make verify`, `make verify-proxy`는 검증 범위 확정 후 실행할 후보 명령이다.

## 이전 Iteration 2 기록

C/POSIX HTTP 서버의 개인 Iteration 2 구현이다. 팀 공통 범위는 docs/DEVELOPMENT_CONTEXT.md와 Issue #2, 개인 선택은 PLAN.md 및 docs/iteration-2/Snoopykid/를 참고한다.

## 실행

Ubuntu WSL2 셸:

```sh
cd /mnt/c/Users/David/SW-AI-W02-03-TEMPLATE/jungle-week-13-c-web-server
make
./build/server 8080
```

서버는 시작 로그 없이 연결을 기다린다. 다른 터미널에서 수동 확인할 수 있다.

```sh
curl --noproxy '*' -i http://127.0.0.1:8080/hello
curl --noproxy '*' -i --data-binary 'hello' http://127.0.0.1:8080/echo
```

GET /와 /hello는 text/plain 및 줄바꿈 없는 Hello 5바이트다. POST /echo는 입력 바이트 그대로, application/octet-stream과 실제 본문 길이로 응답한다. 종료는 서버 터미널의 Ctrl+C다. 이 명령들은 사용 안내이며 이 작업에서 서버·curl을 실행했다는 뜻은 아니다.

## 이번 지원 범위

- IPv4 0.0.0.0, 블로킹 순차 처리, 연결당 요청 하나, Connection: close.
- HTTP/1.1 origin-form만 지원하는 제한 구현이다. 완전한 HTTP/1.1 구현이 아니다. absolute-form은 거부하고 chunked/Transfer-Encoding 요청도 거부한다.
- 첫 물음표 앞 원래 경로로 라우팅한다. 유효한 percent escape 구문은 검사하지만 디코딩·정규화하지 않는다. 정적 파일 설계 전 루트 밖 접근 방지와 함께 재검토한다.
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

- 서버: 현재 Makefile의 -std=c11 -Wall -Wextra -Werror -O2로 컴파일 성공.
- 네 개 단위 테스트 바이너리: -fsanitize=address,undefined로 컴파일 성공. 실행하지 않았으므로 sanitizer 검사 통과를 의미하지 않는다.
- 두 E2E 스크립트: Python 구문 컴파일 성공. 실행하지 않았다.
- 공식 검증·성능 측정·비교 실험: 사용자 범위 확정/허락 전이며 미실행이다.

허락 후 사용할 후보 명령:

```sh
make test
make verify
```

make test는 기존 세 단위 테스트와 test_http_parser를 실행한다. make verify는 갱신한 1단계 E2E와 기본 2단계 E2E를 실행한다. 기본 E2E에는 시간 기한·폐기 상한·할당 실패 주입의 모든 분기가 포함되지 않으므로 전체 완료 증거로 확대하지 않는다. 해당 기본 기능 검증 방법은 별도 범위 검토가 필요하다. 광범위한 조합·퍼징·장시간/경합 회귀는 6단계에서 확대한다. 성능 계측·부하는 별도 허락 대상이다.

## 설명과 기록

- docs/iteration-2/Snoopykid/iteration-2-walkthrough.md: main부터 대화의 결정과 연결한 코드 흐름.
- docs/iteration-2/Snoopykid/iteration-2-requirements.md: 최소 범위·표준·선택 기록.
- docs/iteration-2/Snoopykid/iteration-2-tests.md: 기본 성공·실패 후보와 아직 남은 검증.
- EXPERIMENTS.md: 후속 비교·chunked·URI 설계 검토 목록.
- 이전 개인 구현과 build/legacy/의 보관 바이너리는 현재 Makefile의 빌드 대상이 아니다.

## 빌드 산출물과 라우터 분리
make는 build/server 및 build/obj/*.o를 만든다. 테스트 바이너리도 build/test_*에 생성한다. make test_parse_port 등 기존 개별 빌드 명령은 새 경로 대상의 별칭이다. make clean은 현재 서버·객체·테스트 산출물을 지우며 build/legacy의 보관 파일은 유지한다. build 전체는 Git 추적에서 제외한다.

src/http.c는 구문 해석과 상태 문구, src/router.c는 경로/메서드 판별과 응답 내용 선택, src/server.c는 I/O와 연결 자원 수명을 담당한다. 라우터의 응답 본문은 기존 공간을 빌려 참조한다. 기존 HTTP 동작과 컴파일 옵션을 유지했다.
