# jungle-week-13-c-web-server

C/POSIX HTTP 서버와 Reverse Proxy의 Iteration 5 **B 후보**다. 기본 실행은 고정 네트워크 스레드 + 스레드별 epoll이다. 기준 HTTP·라우팅 계약은 기존 팀 Iteration 4 코드를 유지한다.

## 현재 B 구조

~~~text
main: accept4 → 상한 있는 worker 큐 → eventfd 알림
  ├─ worker 1: epoll → 여러 client/backend의 nonblocking 상태 머신
  ├─ worker 2: epoll → 여러 client/backend의 nonblocking 상태 머신
  └─ ... (기본 4)
파일·CGI 대기: 고정 보조 I/O 스레드 2개 (대기 큐 256)
조회 A/B 선택: 프로세스 전체 RR, 짧은 mutex로 순번만 보호
~~~

작업자 하나가 여러 연결을 이어 처리한다. client/backend socket I/O에는 기존 blocking `handle_client`를 호출하지 않는다. `server.c`의 순차 경로는 기존 단위 테스트와 기준 동작 설명을 위해 남아 있다.

- [요구사항과 선택 이유](docs/iteration-5/jaeseong/iteration-5-requirements.md)
- [요청부터 종료까지 코드 흐름](docs/iteration-5/jaeseong/iteration-5-walkthrough.md)
- [기능 검증 범위와 결과](docs/iteration-5/jaeseong/iteration-5-tests.md)
- [성능 측정 제안](docs/iteration-5/jaeseong/iteration-5-performance-plan.md)
- [B 성능 측정 결과](docs/iteration-5/jaeseong/iteration-5-performance-results.md) — 30회 측정, 준비 구간 실패는 별도 기록

B는 Linux/WSL2의 epoll/eventfd/signalfd를 사용한다. 기존 Ubuntu가 VHDX 오류로 실행되지 않아 이번 개발·검증은 Debian WSL에서 수행하며, 다른 OS의 실행을 보장하지 않는다.

## 빌드와 실행

Ubuntu WSL2에서 프로젝트 루트로 이동한다.

~~~sh
cd "/mnt/c/Users/wotjd/Desktop/정글/week13"
make all backend
~~~

저장소의 `config/proxy.conf.example`을 사용하거나 같은 형식의 설정을 준비한다. 빌드는 이 설정 파일을 덮어쓰지 않는다.

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
./build/server 8080 config/proxy.conf.example
# 네트워크 worker 수를 명시할 때 (1~64, 기본 4):
./build/server 8080 config/proxy.conf.example 4
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
- 백엔드 응답: 평문 HTTP/1.0·1.1을 수용하고 클라이언트 상태 줄은 HTTP/1.1로 구성한다. 상태 코드·상태 문구는 보존한다. HTTP/1.0의 1xx/Transfer-Encoding, 101 Upgrade·잘못된 프레이밍은 502다. 클라이언트 요청과 백엔드로 보내는 요청은 HTTP/1.1을 유지하며 완전한 HTTP 구현을 표방하지 않는다.
- 백엔드 응답은 Content-Length, chunked, 연결 종료 프레이밍을 지원한다. 다른 Transfer-Encoding 체인은 거절한다.
- 상태·전달 대상 헤더·중복 필드·바이너리 본문을 보존한다. Host는 백엔드 주소로 바꾸고 Via를 추가한다. hop-by-hop 및 Connection에서 지명한 필드는 제거한다.
- 프록시 경로에서 Connection이 Host·Content-Length·Transfer-Encoding을 지목하면 본문 추가 수신·100 Continue·백엔드 연결 전에 400으로 거절한다. 일반 지명 헤더 제거와 로컬 경로 정책은 유지한다.
- 백엔드 요청 생성 시 본문과 원래 길이 필드가 없는 GET에는 Content-Length: 0을 합성하지 않는다. 명시적 GET 길이와 빈 POST의 0바이트 길이는 유지한다.
- Expect: 100-continue는 프록시에서 처리하고 백엔드로 재전달하지 않는다. 다른 1xx는 출력 헤더 공간 안에 보관해 최종 헤더 검증 후 전달한다. 중간 응답에는 Connection: close를 생성하지 않고 최종 응답에만 생성한다.
- chunked 응답의 청크 확장은 해석하고 재생성하지 않으며, 유효한 전달 대상 트레일러는 보존한다. 잘못되거나 금지된 트레일러는 불완전 응답으로 종료한다.
- 최종 응답 송신을 시작한 뒤 오류가 발생하면 정리한다. 새 HTTP 오류를 붙이지 않고 chunked의 정상 종료 청크도 생략한다.
- 인증·세션·토큰·권한 관리, 캐싱, 여러 프록시는 추가하지 않는다.

## 임시 기한과 자원 소유권

| 구간 | 임시값 |
| --- | --- |
| 클라이언트 입력/출력 무진행 | 5초 |
| 기존 전체 요청 헤더 / 본문 | 7초 / 15초 |
| 백엔드 연결 전체 | 1초 |
| 백엔드 요청 전달 전체 | 5초 |
| 요청 전달 후 백엔드 최종 헤더 전체 | 1초 |
| 백엔드 본문 무진행 / 전체 수신 | 5초 / 15초 |
| 기존 응답 후 수신 폐기 | 최대 1초 / 64KiB |

수치는 표준 권고값·최적값이 아니다. B의 본문 전체 기한은 최종 헤더 검증 뒤 시작하고, 클라이언트 송신 대기도 포함한다. 백엔드 무진행 기한은 읽기를 기다리는 동안 적용하며 클라이언트 역압 후 다시 시작한다. 각 기한은 작업자의 최소 힙에 등록하고 epoll 대기 시간에 반영한다.

main은 설정·리스너와 전체 실행 수명을 소유한다. 수락 큐에 전달한 뒤 client/backend FD·요청 버퍼·송신 위치의 소유자는 해당 네트워크 worker다. 응답 헤더 작업 공간은 nonblocking 수신을 이어갈 수 있도록 연결에 보관하고, 헤더 송신 완료 뒤 회수한다. 파일/CGI 보조 작업 동안에는 연결을 해제하지 않는다. 종료 시 FD 등록 제거·close를 먼저 하고 같은 이벤트 배치의 포인터 사용이 끝난 뒤 메모리를 해제한다.

16KiB는 스트림 버퍼 용량이다. 헤더·트레일러·요청 본문과 구조체 공간이 추가되므로 연결 전체 메모리와 같지 않다. 응답 헤더/청크 메타데이터는 기존 8KiB 상한을 재사용하며, 출력 헤더 공간은 필드 재구성과 Via 등을 위해 그 상한에서 유도한다.

## 컴파일과 공식 검증

B의 이번 실행 결과는 [Iteration 5 검증 기록](docs/iteration-5/jaeseong/iteration-5-results.md)을 참조한다. 과거 Iteration 4의 검증/성능 기록은 해당 구현 당시 결과이며 현재 B의 결과로 간주하지 않는다.

~~~sh
# 아래 두 명령은 사용자와 검증 범위를 확정한 후 실행한다.
make test
make verify
make verify-b
~~~

make test: 기존 8개 + test_event_io (ASan/UBSan). test_event_io는 실제 EAGAIN/부분 송신과 타이머 힙을 검사한다. 수명 테스트는 별도 build/test-obj 객체에서만 할당 함수를 대체하며 서버에는 주입 코드를 넣지 않는다.

make verify: Iteration 1·2·3 정적·3 동적 회귀와 Iteration 4 C 백엔드 중계 및 엣지 40개 검증. make verify-edge로 엣지 스크립트만 실행할 수도 있다. 테스트는 임시 설정·정적 fixture·임시 포트를 사용한다. 사용자 설정 파일과 실제 운영 권한은 바꾸지 않는다.

빌드 산출물은 build/이며 C CGI 바이너리는 기존 cgi-bin/을 사용한다. 둘 다 Git에서 제외한다. make clean은 명시한 바이너리·객체만 지우며 build/proxy.conf.example과 build/legacy/는 유지한다.


## 과거 Iteration 4 승인 성능 재측정 기록 (현재 B 측정 아님)

2026-11-25 동일 WSL에서 f32e17d와 현재 스냅샷을 공통 C 백엔드/1KiB 응답/-O2로 비교했다. 직접 기준선과 S1~S6/S_MAX, 조건별3회로 총48회를 완료했다. 성공165,929건, 실패·5xx0건, S1 양쪽 통과, 예정 요청 건수 미달·FD증가0이다.

포화 조회 처리량 중앙값은3,920.8→3,859.9req/s(-1.55%)로 관측됐으며 최소~최대 범위가 겹친다. 기존 외부 Linux/Go의4,937req/s와 직접 비교하거나 확정적인 성능 개선/저하로 해석하지 않는다. CPU·RSS·p50/p95·조건/한계는 [성능 보고서](docs/iteration-4/Snoopykid/iteration-4-performance-results.md), 반복별 데이터는 [CSV](docs/iteration-4/Snoopykid/performance/trials.csv)에 있다. 성능 결과에 따른 자동 최적화는 하지 않았다.

## 코드 흐름과 후속 범위

- 현재: main → run_event_server → 수락 큐/eventfd → run_worker_event_loop → advance_connection → 기존 파서/라우터 → event_proxy의 nonblocking 백엔드 처리 → 응답/정리.
- 상세 리뷰: [Iteration 5 호출 흐름](docs/iteration-5/jaeseong/iteration-5-walkthrough.md).
- 기존 server.c의 handle_client/proxy_prepare_response는 순차 기준/단위 테스트 경로이며 현재 main에서 호출하지 않는다. 과거 설명은 docs/iteration-4/Snoopykid/iteration-4-walkthrough.md다.
- 기본 스레드는 메인 1 + 네트워크 4 + 파일/CGI 보조 2 = 7개다. 리스너는 메인만 수락하고 연결은 한 작업자가 종료까지 담당한다.
- 동시 연결 1만 개는 검토 목표다. 성능 측정은 환경·조건·지표를 제시하고 별도 허락 후 진행한다.
- 기존 정적 파일의 realpath/open 사이 경합 등 운영상 한계는 이번 프록시 추가로 해결했다고 주장하지 않는다.

## B 성능 재측정

가독성 정리 후 현재 코드로 기존 조건 30회를 다시 측정했다. [현재 코드 성능 결과](docs/iteration-5/jaeseong/iteration-5-performance-retest-results.md)에 환경·조건별 중앙값/범위·실패·RSS·FD·원본 자료를 기록했다. 측정 144,000건·준비 28,800건 모두 성공했다. 추가 조건은 [재측정 계획](docs/iteration-5/jaeseong/iteration-5-performance-retest-plan.md)에서 사용자 선택을 기다리고 있다. 이전 [리팩토링 전 측정](docs/iteration-5/jaeseong/iteration-5-performance-results.md)도 보존한다.
