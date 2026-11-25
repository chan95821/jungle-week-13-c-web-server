# jungle-week-13-c-web-server

C/POSIX HTTP 서버와 Reverse Proxy다. 제품 main에서 부모 리스너 하나와 고정 작업자 프로세스 4개를 실행한다. 각 작업자는 LT epoll로 여러 클라이언트·백엔드·CGI 파이프를 관리하고, 정적 파일 작업은 제한된 보조 스레드에 맡긴다. 최소 통합 확인을 통과했으며 전체 회귀·광범위한 장애 검증은 다음 체크포인트에 남아 있다.

## 빌드와 실행

Ubuntu WSL2에서 프로젝트 루트로 이동한다.

~~~sh
cd /mnt/c/Users/David/SW-AI-W02-03-TEMPLATE/jungle-week-13-c-web-server
make all backend
~~~

기존 개인 설정 build/proxy.conf.example은 자동 변경하지 않는다. 현재는 worker-policy와 query-policy도 각각 필수다. 아래 RR 값은 실행 예시이며 최종 알고리즘 선정이 아니다. 저장소의 config/proxy.conf.example을 사용할 수 있다. build/는 Git에서 제외되며 빌드와 clean은 개인 설정을 덮어쓰거나 삭제하지 않는다.

~~~text
submission 127.0.0.1:8001
query-a 127.0.0.1:8002
query-b 127.0.0.1:8003
~~~

빈 줄과 # 주석을 허용한다. 세 역할과 두 정책이 각각 한 번 필요하다. 정책 값은 rr, least-outstanding, response-time, combined다. 호스트명·IPv6·누락·중복·잘못된 포트·추가 토큰은 거절한다. 주소는 시작 시 읽고 실행 중 재설정하지 않는다.

테스트 백엔드 세 개를 각각 별도 터미널에서 실행한다. 백엔드 바이너리는 테스트 코드에만 별도 진입점을 두고 기존 C 리스너·파서·송수신 코드를 재사용한다.

~~~sh
./build/test_backend 8001 submission
./build/test_backend 8002 query-a
./build/test_backend 8003 query-b
~~~

다른 터미널에서 프록시를 실행한다.

~~~sh
./build/server 8080 config/proxy.conf.example
~~~

프록시는 public 디렉터리가 있는 프로젝트 루트에서 실행한다. 시작 로그 없이 기다리며 종료는 Ctrl+C다. 클라이언트 접속 주소·포트는 하나다.

~~~sh
curl --noproxy '*' -i http://127.0.0.1:8080/api/hello
curl --noproxy '*' -i http://127.0.0.1:8080/api/submission-status
curl --noproxy '*' -i --data-binary 'hello' http://127.0.0.1:8080/api/submissions
curl --noproxy '*' -i http://127.0.0.1:8080/hello
~~~

위 RR 예시에서는 조회 응답의 X-Backend-ID가 query-a/query-b로 순환한다. 제출은 submission에서 201과 받은 바이트를 반환한다. 이 명령은 수동 사용 안내이며 실제 실행·검증 통과 결과가 아니다.

시간 기반 정책은 EWMA 1/8과 매 32번째 RR 탐색을 사용하고, 결합 점수는 (진행 수+1)×대표 시간이다. worker-policy는 부모의 작업자 배정, query-policy는 작업자의 공유 상태 기반 조회 백엔드 선택에 적용된다. 각 작업자는 여러 연결을 관리하며, 최종 알고리즘은 아직 비교 후 선택할 사항이다. 세부 정책과 한계는 [정책 설계](docs/iteration-5/Snoopykid/iteration-5-policy-rules-design.md)를 참고한다.

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

main이 설정·조회 순번·리스너·클라이언트 FD를 소유한다. handle_client가 요청 헤더·요청 본문을 소유한다. 프록시 응답 상태는 백엔드 FD·16KiB 수신 버퍼와 필요한 길이만 할당한 송신 헤더·Connection 필드·트레일러를 소유한다. 원본 헤더와 재구성 작업 공간은 헤더 준비 함수의 지역 공간이다. 송신 헤더는 첫 본문 next에서 회수하고, 그 외 정상·실패 경로는 provider cleanup이 회수한다. 준비 실패 시 준비 함수가 회수한다.

16KiB는 스트림 버퍼 용량이다. 헤더·트레일러·요청 본문과 구조체 공간이 추가되므로 연결 전체 메모리와 같지 않다. 응답 헤더/청크 메타데이터는 기존 8KiB 상한을 재사용하며, 출력 헤더 공간은 필드 재구성과 Via 등을 위해 그 상한에서 유도한다.

## 컴파일과 공식 검증

2026-11-25 P2-9 A안 라우팅 일원화 후 clean·컴파일/링크 성공, 단위 8개(ASan/UBSan)와 E2E 스크립트 6개를 통과했다. 엣지 40개 통과·실패 0·skip 0이며 전체 최종 실패·skip도 없었다. 기존 정책과 오류 우선순위를 유지했다. 최신 상세 기록은 docs/iteration-4/Snoopykid/iteration-4-routing-verification-results.md다. 이전 보완 보고서는 당시 기록으로 보존한다. 기능 검증 이후 사용자 승인 조건의 보완 전후 성능48회를 별도로 완료했다. 외부 교차 스위트 전체는 재실행하지 않았다.

~~~sh
# 아래 두 명령은 사용자와 검증 범위를 확정한 후 실행한다.
make test
make verify
~~~

make test: test_parse_port, test_recv_header, test_send_all, test_http_parser, test_body_send, test_proxy_selection, test_proxy_protocol, test_proxy_lifecycle (ASan/UBSan). 수명 테스트는 별도 build/test-obj 객체에서만 할당 함수를 대체하며 서버에는 주입 코드를 넣지 않는다.

make verify: Iteration 1·2·3 정적·3 동적 회귀와 Iteration 4 C 백엔드 중계 및 엣지 40개 검증. make verify-edge로 엣지 스크립트만 실행할 수도 있다. 테스트는 임시 설정·정적 fixture·임시 포트를 사용한다. 사용자 설정 파일과 실제 운영 권한은 바꾸지 않는다.

빌드 산출물은 build/이며 C CGI 바이너리는 기존 cgi-bin/을 사용한다. 둘 다 Git에서 제외한다. make clean은 명시한 바이너리·객체만 지우며 build/proxy.conf.example과 build/legacy/는 유지한다.


## 승인 성능 재측정 결과

2026-11-25 동일 WSL에서 f32e17d와 현재 스냅샷을 공통 C 백엔드/1KiB 응답/-O2로 비교했다. 직접 기준선과 S1~S6/S_MAX, 조건별3회로 총48회를 완료했다. 성공165,929건, 실패·5xx0건, S1 양쪽 통과, 예정 요청 건수 미달·FD증가0이다.

포화 조회 처리량 중앙값은3,920.8→3,859.9req/s(-1.55%)로 관측됐으며 최소~최대 범위가 겹친다. 기존 외부 Linux/Go의4,937req/s와 직접 비교하거나 확정적인 성능 개선/저하로 해석하지 않는다. CPU·RSS·p50/p95·조건/한계는 [성능 보고서](docs/iteration-4/Snoopykid/iteration-4-performance-results.md), 반복별 데이터는 [CSV](docs/iteration-4/Snoopykid/performance/trials.csv)에 있다. 성능 결과에 따른 자동 최적화는 하지 않았다.

## 코드 흐름과 후속 범위

- src/main.c → src/server.c의 handle_client → src/router.c의 단일 경로/메서드 판별 → src/proxy.c의 보호 헤더 검사·설정·백엔드 분배 → src/proxy_transfer.c의 조율 → proxy_io.c의 연결/수신과 proxy_headers.c의 요청/응답 헤더 준비 → proxy_body.c의 본문 제공 → 기존 송신·정리.
- 상세 리뷰: docs/iteration-4/Snoopykid/iteration-4-walkthrough.md.
- 선택·검증 후보: docs/iteration-4/Snoopykid/iteration-4-requirements.md, iteration-4-tests.md.
- 여러 작업자가 각자 여러 연결을 관리한다. 실제 처리 능력과 1만 연결 가능 여부는 아직 측정하지 않았다.
- Iteration 5는 고정 프로세스 작업자 + 작업자별 LT epoll이다. 부모가 accept/FD 배정, 작업자가 공유 상태로 백엔드를 선택한다. 파일 provider는 helper, CGI 파이프는 이벤트 루프가 담당한다.
- 동시 연결 1만 개는 검토 목표다. 성능 측정은 환경·조건·지표를 제시하고 별도 허락 후 진행한다.
- 기존 정적 파일의 realpath/open 사이 경합 등 운영상 한계는 이번 프록시 추가로 해결했다고 주장하지 않는다.
## Iteration 5 worker 네트워크 구성요소

`make worker-io`는 실제 nonblocking worker reactor를 일반 옵션으로 컴파일한다. 현재 `./build/server`의 main은 부모/4작업자 실행 경로에 연결돼 있다. 정적 파일은 helper, CGI는 비차단 stdout/자식 회수 흐름을 사용한다.

worker reactor는 기존 FD STREAM 수신, 여러 TCP 클라이언트·backend의 LT epoll, 16KiB relay와8KiB 재개점, 정확한 HTTP framing,10ms 공유 잠금 재시도, 선택잠금1초503, 기존 전체기한을 구현한다. 다음은 C backend와 fork worker를 사용하는 기능 검증 명령이며 부하·성능 비교가 아니다.

```sh
make test_worker_http_io test_worker_io_state
./build/test_worker_http_io
./build/test_worker_io_state
```

첫 시험은 실제 TCP·프로세스·역압력, 두 번째는 주입 시각의 기한 경계·이벤트 예산을 검증한다. 상세 함수 흐름과 미통합 범위는 [worker I/O 리뷰](docs/iteration-5/Snoopykid/iteration-5-worker-io-review.md)를 참고한다.

### Iteration 5 통합 개발용 최소 확인

`make smoke-process-http`는 부모 리스너 하나·실제 작업자 4개·C 테스트 백엔드로 정상 응답, 느린/빠른 요청의 동시 진행, 종료·자원 회수를 확인합니다. 일반 strict O2 빌드이며 전체 회귀·sanitizer·성능 검사를 대신하지 않습니다. 현재 제품 `build/server`의 main도 부모/4작업자 실행 경로를 사용합니다. 제품 진입점·CGI 최소 확인은 `make smoke-process-server`입니다.

[부모 HTTP 통합 리뷰](docs/iteration-5/Snoopykid/iteration-5-parent-integration-review.md)

파일 보조 스레드 최소 확인은 `make smoke-file-io`, 실제 부모/4작업자·정적 파일/프록시 통합 최소 확인은 `make smoke-process-http`입니다. 임시 fixture를 사용합니다. [파일 I/O 구현 리뷰](docs/iteration-5/Snoopykid/iteration-5-file-io-review.md)에 함수 흐름·소유권·종료 한계·실행 결과를 기록했습니다. CGI와 제품 main 연결은 완료했으며, 전체 회귀와 광범위한 장애 검증은 통합 체크포인트에 남아 있습니다.

### 현재 실행 및 CGI 통합 확인

프로젝트 루트의 Ubuntu/WSL 터미널에서 실행합니다.

```sh
make
./build/server 8080 config/proxy.conf.example
# 다른 터미널
curl --noproxy '*' -i http://127.0.0.1:8080/hello
curl --noproxy '*' -i 'http://127.0.0.1:8080/cgi-bin/order'
# 서버 터미널에서 Ctrl+C로 종료
```

예제 설정은 worker/query RR을 명시한 실행 예이며 최종 알고리즘 결정이 아닙니다. 프록시 요청에는 예제에 지정한 C 백엔드를 별도로 실행해야 합니다. 개인 `build/proxy.conf.example`은 수정하지 않았습니다.

`make smoke-process-server`는 임시 정적 파일과 C 백엔드/CGI를 사용해 제품 실행 경로의 정상·느린 CGI 중 다른 요청·취소·종료 정리를 확인합니다. 부하/성능 시험이 아닙니다. [CGI·main 통합 리뷰](docs/iteration-5/Snoopykid/iteration-5-cgi-main-review.md)를 참고하세요.
