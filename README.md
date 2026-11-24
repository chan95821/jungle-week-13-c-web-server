# jungle-week-13-c-web-server

C/POSIX HTTP 서버와 순차 Reverse Proxy의 Iteration 4 개인 구현이다. 기준은 Issue #4·#17과 현재 팀의 정적·동적 응답 코드다. 동시성은 Iteration 5에서 구현한다.

## 빌드와 실행

Ubuntu WSL2에서 프로젝트 루트로 이동한다.

~~~sh
cd /mnt/c/Users/David/SW-AI-W02-03-TEMPLATE/jungle-week-13-c-web-server
make all backend
~~~

사용자가 옮긴 설정 파일은 build/proxy.conf.example이다. 파일 내용은 아래 형식이다. 빌드와 clean은 이 파일을 덮어쓰거나 삭제하지 않는다. build/는 Git에서 제외되므로 새 체크아웃에서는 설정 파일을 직접 준비해야 한다.

~~~text
submission 127.0.0.1:8001
query-a 127.0.0.1:8002
query-b 127.0.0.1:8003
~~~

빈 줄과 # 주석을 허용한다. 세 역할이 각각 한 번 필요하다. 호스트명·IPv6·누락·중복·잘못된 포트·추가 토큰은 거절한다. 주소는 시작 시 읽고 실행 중 재설정하지 않는다.

테스트 백엔드 세 개를 각각 별도 터미널에서 실행한다. 백엔드 바이너리는 테스트 코드에만 별도 진입점을 두고 기존 C 리스너·파서·송수신 코드를 재사용한다.

~~~sh
./build/test_backend 8001 submission
./build/test_backend 8002 query-a
./build/test_backend 8003 query-b
~~~

다른 터미널에서 프록시를 실행한다.

~~~sh
./build/server 8080 build/proxy.conf.example
~~~

프록시는 public 디렉터리가 있는 프로젝트 루트에서 실행한다. 시작 로그 없이 기다리며 종료는 Ctrl+C다. 클라이언트 접속 주소·포트는 하나다.

~~~sh
curl --noproxy '*' -i http://127.0.0.1:8080/api/hello
curl --noproxy '*' -i http://127.0.0.1:8080/api/submission-status
curl --noproxy '*' -i --data-binary 'hello' http://127.0.0.1:8080/api/submissions
curl --noproxy '*' -i http://127.0.0.1:8080/hello
~~~

조회 응답의 X-Backend-ID가 query-a/query-b로 순환한다. 제출은 submission에서 201과 받은 바이트를 반환한다. 이 명령은 수동 사용 안내이며 실제 실행·검증 통과 결과가 아니다.

## 라우팅과 HTTP 범위

| 요청 | 목적지 |
| --- | --- |
| POST /api/submissions | 제출 백엔드 |
| GET /api/submission-status | 조회 A/B |
| GET /api/hello | 같은 조회 A/B 풀 |
| 기존 /, /hello, /echo, /cafe, /menu, /assets/, /greet, /cgi-bin/ | 기존 로컬 처리 |

조회는 선택 직후 순번을 넘기며 실패해도 같은 요청을 다른 서버에 자동 재시도하지 않는다. 제출 요청은 조회 순번을 소비하지 않는다. 경로·쿼리 바이트는 재작성하지 않는다.

- 클라이언트: 기존 HTTP/1.1 origin-form, GET/POST, 요청 줄·헤더 8KiB, 본문 64KiB, 엄격한 Content-Length 정책을 유지한다. 요청 chunked·absolute-form·keep-alive는 추가하지 않는다. HEAD는 기존 501과 본문 생략 정책이다.
- 백엔드: 평문 HTTP/1.1만 받는다. HTTP/1.0·101 Upgrade·잘못된 프레이밍은 502다. 완전한 HTTP/1.1 구현을 표방하지 않는다.
- 백엔드 응답은 Content-Length, chunked, 연결 종료 프레이밍을 지원한다. 다른 Transfer-Encoding 체인은 거절한다.
- 상태·전달 대상 헤더·중복 필드·바이너리 본문을 보존한다. Host는 백엔드 주소로 바꾸고 Via를 추가한다. hop-by-hop 및 Connection에서 지명한 필드는 제거한다.
- Expect: 100-continue는 프록시에서 처리하고 백엔드로 재전달하지 않는다. 다른 1xx는 출력 헤더 공간 안에 보관해 최종 헤더 검증 후 전달한다.
- chunked 응답의 청크 확장은 해석하고 재생성하지 않으며, 유효한 전달 대상 트레일러는 보존한다. 잘못되거나 금지된 트레일러는 불완전 응답으로 종료한다.
- 최종 응답 송신을 시도한 뒤 오류가 발생하면 기록·정리만 한다. 새 HTTP 오류를 붙이지 않고 chunked의 정상 종료 청크도 생략한다.
- 인증·세션·토큰·권한 관리, 캐싱, 여러 프록시는 추가하지 않는다.

## 임시 기한과 자원 소유권

| 구간 | 임시값 |
| --- | --- |
| 기존 클라이언트 I/O 호출 | 5초 |
| 기존 전체 요청 헤더 / 본문 | 7초 / 15초 |
| 백엔드 연결 전체 | 1초 |
| 백엔드 요청 전달 전체 | 5초 |
| 요청 전달 후 백엔드 최종 헤더 전체 | 1초 |
| 백엔드 본문 무진행 / 전체 수신 | 5초 / 15초 |
| 기존 응답 후 수신 폐기 | 최대 1초 / 64KiB |

수치는 표준 권고값·최적값이 아니다. 응답 본문 수신 기한은 최종 헤더 검증 뒤 시작하고 수신 진행으로 늘리지 않는다. 클라이언트에 마지막 바이트가 도달할 때까지의 전체 완료 기한을 보장하는 것은 아니다. 기존 클라이언트 송신 호출 제한은 유지한다.

main이 설정·조회 순번·리스너·클라이언트 FD를 소유한다. handle_client가 요청 헤더·요청 본문을 소유한다. 프록시 응답 상태는 백엔드 FD·응답 헤더·16KiB 스트림 버퍼·트레일러 공간을 소유하며 provider cleanup에서 해제한다. 준비 실패 시 준비 함수가 회수한다.

16KiB는 스트림 버퍼 용량이다. 헤더·트레일러·요청 본문과 구조체 공간이 추가되므로 연결 전체 메모리와 같지 않다. 응답 헤더/청크 메타데이터는 기존 8KiB 상한을 재사용하며, 출력 헤더 공간은 필드 재구성과 Via 등을 위해 그 상한에서 유도한다.

## 컴파일과 공식 검증

이 작업에서는 서버·C 테스트 백엔드·7개 단위 테스트를 컴파일·링크하고 Python 스크립트의 구문을 확인한다. 공식 테스트·부하·성능 측정은 별도 범위 확정 전에는 실행하지 않는다. sanitizer로 컴파일한 사실은 검사 통과가 아니다.

~~~sh
# 아래 두 명령은 사용자와 검증 범위를 확정한 후 실행한다.
make test
make verify
~~~

make test: test_parse_port, test_recv_header, test_send_all, test_http_parser, test_body_send, test_proxy_selection, test_proxy_protocol (ASan/UBSan).

make verify: Iteration 1·2·3 정적·3 동적 회귀와 Iteration 4 C 백엔드 중계 검증. 테스트는 임시 설정·정적 fixture·임시 포트를 사용한다. 사용자 설정 파일과 실제 운영 권한은 바꾸지 않는다.

빌드 산출물은 build/이며 C CGI 바이너리는 기존 cgi-bin/을 사용한다. 둘 다 Git에서 제외한다. make clean은 명시한 바이너리·객체만 지우며 build/proxy.conf.example과 build/legacy/는 유지한다.

## 코드 흐름과 후속 범위

- src/main.c → src/server.c의 handle_client → src/proxy.c의 설정·목적지·분배 → src/proxy_transfer.c의 연결·요청·응답 준비 → 기존 provider 송신·정리.
- 상세 리뷰: docs/iteration-4/Snoopykid/iteration-4-walkthrough.md.
- 선택·검증 후보: docs/iteration-4/Snoopykid/iteration-4-requirements.md, iteration-4-tests.md.
- 순차 처리이므로 조회 A/B 분배가 동시 처리 효과를 보장하지 않는다.
- Iteration 5는 여러 작업자 + 작업자별 non-blocking 이벤트 루프 방향이다. 프로세스/스레드·수락/분배·상태 공유·역압력 정책은 사용자 선택 전이다. 현재 blocking provider를 그대로 이벤트 루프에서 호출하지 않는다.
- 동시 연결 1만 개는 검토 목표다. 성능 측정은 환경·조건·지표를 제시하고 별도 허락 후 진행한다.
- 기존 정적 파일의 realpath/open 사이 경합 등 운영상 한계는 이번 프록시 추가로 해결했다고 주장하지 않는다.