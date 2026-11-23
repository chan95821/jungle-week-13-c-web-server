# jungle-week-13-c-web-server

C/POSIX 웹 서버의 개인 Iteration 3 구현이다. 최신 원격 main과 동일했던 06a803a에서 로컬 iteration/3-static-dynamic → team/Snoopykid/iteration-3-static-dynamic으로 분기했다. 팀 범위는 Issue #3, 개인 선택은 PLAN.md 및 docs/iteration-3/Snoopykid/에 기록한다.

## 실행

Ubuntu WSL2 셸에서 정적 루트를 반드시 지정한다.

~~~sh
cd /mnt/c/Users/David/SW-AI-W02-03-TEMPLATE/jungle-week-13-c-web-server
make
./build/server 8080 ./public
~~~

시작 로그 없이 연결을 기다린다. 루트를 열 수 없거나 디렉터리가 아니면 오류 출력 후 시작을 중단한다. 종료는 서버 터미널에서 Ctrl+C다. 다음은 수동 확인용 안내이며 이 작업에서 실행한 명령은 아니다.

~~~sh
curl --noproxy '*' -i http://127.0.0.1:8080/static/example.html
curl --noproxy '*' -i 'http://127.0.0.1:8080/greet?name=Alice'
curl --noproxy '*' -i http://127.0.0.1:8080/hello
curl --noproxy '*' -i --data-binary 'hello' http://127.0.0.1:8080/echo
~~~

브라우저: http://127.0.0.1:8080/static/example.html
파일명에 공백/한글이 있으면 URL 인코딩한다. curl로 점 경로 거절을 확인할 때는 클라이언트가 경로를 먼저 정리하지 않도록 --path-as-is를 사용한다.

## 라우팅·정적 파일

- 기존 GET /·/hello: text/plain, 줄바꿈 없는 Hello 5바이트. POST /echo: application/octet-stream, 요청 본문 바이트 그대로.
- GET /static/a.css는 루트의 a.css다. 디렉터리/끝 / 요청은 403, /static은 404, 반복 슬래시는 400. 목록·index.html 자동 제공·리다이렉트는 없다.
- 정적 경로만 %HH를 한 번 디코딩한다. 인코딩된 슬래시·역슬래시·NUL·바이트 제어문자는 400. .·.. 세그먼트는 403. +는 정적 파일명에서 그대로다.
- 루트는 시작 시 FD로 열어 유지하며 설정 경로의 링크는 허용한다. 요청 경로는 각 요소를 openat/O_NOFOLLOW로 열어 중간·마지막 링크를 모두 거절한다.
- 일반 파일만 제공한다. 없음/중간 요소가 파일이면 404, 권한·링크·비정규 파일은 403, OS 이름 길이 초과 414, 그 밖의 내부 오류 500.
- 정적 경로와 /greet는 GET만 지원한다. POST는 파일 존재 검사 전에 405 + Allow: GET, 기존 미구현 메서드는 501이다.
- MIME: html/htm=text/html, css=text/css, js=text/javascript, png=image/png, jpg/jpeg=image/jpeg. 대소문자 없이 비교하고 나머지는 application/octet-stream이다. 내용 추정은 하지 않는다.
- fstat한 최초 크기를 Content-Length로 보내고 지역 16KiB 버퍼로 read/send를 반복한다. 파일 전체 malloc·mmap·성능 비교는 구현하지 않았다.
- 증가분은 보내지 않고 조기 EOF·읽기·송신 오류는 로그 후 정리한다. 최종 헤더 이후 두 번째 오류 응답을 붙이지 않는다. 내용 스냅샷은 보장하지 않는다.

## 동적 /greet

GET /greet?name=Alice → text/plain; charset=utf-8 및 Hello, Alice!와 줄바꿈이다.

원래 쿼리를 &와 첫 =로 먼저 나눈 뒤 각 키·값을 %HH 한 번, +→공백으로 해석한다. 디코딩한 키로 name과 중복을 판별한다. name 누락은 World, 빈 값·중복은 400. 키 대소문자를 구분하고 유효한 미지원 키는 의미를 무시한다.

모든 키·값은 현재 UTF-8 검사와 U+0000~001F/U+007F~009F 금지를 적용한다. 과잉 인코딩·대리 코드 포인트·범위 초과·불완전 UTF-8은 400이다. 정규화·공백 제거는 하지 않는다. 인코딩/제어문자 범위의 확대 여부는 검증 체크포인트에서 다시 선택하며 정적 파일 경로에 자동 적용하지 않는다. 별도 name 상한 없이 기존 8KiB 요청 줄·헤더 범위 안에서 처리하고 생성 본문은 필요한 크기로 한 번 할당한다.

## 소유권·임시값

main은 리스너·클라이언트·루트 FD를 정리한다. handle_client는 요청/생성 본문과 파일 FD를 정리하며 하위 파서·송신 함수는 빌려 쓴다. static_open_file은 중간 디렉터리 FD를 정리하고 마지막 일반 파일 FD만 넘긴다.

| 위치 | 임시 정책 |
| --- | --- |
| src/http.h | 요청 줄·헤더 8192바이트, 요청 본문 65,536바이트 |
| src/server.h | 파일 전송 버퍼 16KiB, backlog 128 |
| src/server.h | 수신·송신 호출별 5초, 전체 헤더 7초, 전체 본문 15초 |
| src/server.h | 응답 후 폐기 전체 1초 또는 64KiB |

모든 숫자는 실험 전 초기값이다. IPv4 0.0.0.0·블로킹 순차·연결당 요청 하나·Connection: close를 유지한다. 느린 클라이언트는 다음 연결 처리를 지연시킨다. HTTP/1.1 origin-form 제한 구현이며 완전한 HTTP/1.1 지원이 아니다. chunked·keep-alive·CGI·Reverse Proxy는 이번 범위가 아니다. HEAD는 501이지만 응답 본문을 보내지 않는다. 전체 응답 송신·디스크 읽기 완료 기한은 추가하지 않았다.

## 게시의 신뢰 경계

서버는 읽기/탐색만, 신뢰하는 게시자만 정적 파일·디렉터리를 수정해야 한다. 기존 inode를 덮어쓰지 않고 같은 파일시스템의 공개되지 않는 staging 위치에서 새 파일을 완성·닫은 뒤 rename으로 교체한다. 이전 열린 FD는 이전 파일을 읽는다. 임시 공간·게시 권한·메타데이터 관리가 필요하며, 재부팅 후 내구성은 별도 요구다.

이 운영 조건을 실제 환경에서 확인하지 않았다. 권한 자동 변경·게시 스크립트·콘텐츠 신뢰성 보증은 없다. 악의적인 디렉터리 이동/교체·하드링크/마운트 변경·권한 있는 게시자의 악성 콘텐츠까지 격리한다고 주장하지 않는다. 상세 절차는 docs/iteration-3/Snoopykid/iteration-3-publishing.md를 참고한다.

## 빌드·검증 상태

make는 build/server, build/obj/*.o를 생성한다. 일곱 단위 테스트도 build/test_*에 생성한다. make clean은 현재 대상만 지우고 build/legacy 보관 파일은 유지한다. build 전체는 Git에서 무시한다.

~~~sh
# 컴파일만: 테스트는 실행하지 않는다.
make all test_parse_port test_recv_header test_send_all test_http_parser test_static test_greet test_file_stream

# 아래 두 명령은 2026-11-23 확정 범위로 실행·통과했다.
make test
make verify
~~~

2026-11-23 사용자 확정 범위에서 make test의 일곱 단위 테스트(ASan/UBSan)와 make verify의 세 E2E를 모두 통과했다. 실패·skip은 없었다. E2E 서버는 -O2이며 단위 sanitizer에서 오류 보고는 없었다. 성능 측정과 실제 운영 권한/ACL 확인은 하지 않았다.

make test는 기존 네 테스트와 정적 경로/파일·greet·결정적 파일 송신 실패 테스트를 실행한다. make verify는 이전 두 단계와 이번 기본 E2E를 실행한다. Linux /proc·권한에 의존하는 항목과 조건부 skip이 있어 결과에서 미검증을 구분해야 한다. JPG fixture는 MIME/바이트 전달용이며 이미지 디코더/브라우저 렌더링 검증은 별도다.

기본 공식 실행은 현재 인코딩/제어문자 기준으로 사용자 확정 후 통과했다. 범위 확대는 후속 선택이다. 광범위한 조합·장애·경합은 Iteration 6에서 확대하고 성능 측정은 별도 명시적 허락을 받는다.

## 함수 흐름·선택 기록

- docs/iteration-3/Snoopykid/iteration-3-walkthrough.md: main부터 실제 함수 호출, API·FD·메모리 소유권.
- docs/iteration-3/Snoopykid/iteration-3-requirements.md: 선택 이력과 범위.
- docs/iteration-3/Snoopykid/iteration-3-tests.md: 추천 범위와 통과 기준·제외 항목.
- EXPERIMENTS.md: 버퍼/mmap·greet 입력과 비용의 후속 검토 목록.
검증 상세: docs/iteration-3/Snoopykid/iteration-3-verification-results.md. 실행한 범위의 결과이며 모든 경합·운영 보안·성능·팀 통합 완료를 의미하지 않는다.