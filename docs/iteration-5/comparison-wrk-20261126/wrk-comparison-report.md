# Iteration 5: wrk 제출·조회 혼합 측정과 별도 안정성 검증

2026-11-26. 측정 대상은 jaeseong 0723287, chan af48c91, YYJ c0e3527의 격리 빌드. 제품 소스는 변경하지 않았다.

## 성능: wrk 혼합 요청

wrk 4.1.0, 4 생성 스레드(동시 연결 1일 때 1), 4 서버 작업자, 각 30초 3회. 각 클라이언트는 POST /api/submissions 1KiB binary와 GET /api/submission-status를 번갈아 보낸다. 각 요청은 새 연결이며 응답의 상태·바이트·X-Backend-ID를 검증한다. 단일 연결에서는 두 요청이 순차로 번갈아 진행된다. 제출 서버 1개·조회 A/B 각 1개는 동일 C 테스트 서버이며 각 서버는 순차 처리한다. 따라서 서버 또는 백엔드가 병목일 수 있다. 무작위 순서 교차 반복·직접 백엔드 기준선은 측정하지 않았다.

AMD Ryzen 7 4800H, WSL2 Ubuntu 16 논리 CPU, WSL RAM 약 7.5GiB, FD 제한 10240, GCC 11.4.0 -O2. 부하 생성기·프록시·백엔드가 같은 호스트를 공유한다. 최대 1000 연결까지 측정, 10000 연결 미실행.

아래 성공 RPS는 검증된 제출+조회 응답 수를 wrk가 보고한 실제 경과 시간으로 나눈 값의 3회 중앙값. 실패 응답은 제외한다. 잘못된 응답은 각 반복 합계이며 소켓 오류와 별개다. wrk p99는 모든 완료 HTTP 응답을 포함하며 요청 종류·성공/실패별로 분리되지 않는다. wrk 기본 출력은 p95를 제공하지 않으므로 p95 미측정이다. 테스트 중 진행 중인 요청은 종료 시 집계되지 않을 수 있어 전체 실패율을 단정하지 않는다.

| 동시 연결 | jaeseong 성공 RPS / p99 | chan 성공 RPS / p99 | YYJ 성공 RPS / p99 |
|--:|--:|--:|--:|
|1|2,135 / 692.00us, 694.00us, 710.00us|2,027 / 811.00us, 681.00us, 694.00us|793 / 1.91ms, 1.91ms, 1.93ms|
|10|13,509 / 767.00us, 753.00us, 776.00us|10,155 / 0.95ms, 1.11ms, 1.13ms|5,409 / 10.86ms, 10.77ms, 10.77ms|
|100|21,350 / 7.85ms, 7.41ms, 7.46ms|9,607 / 15.88ms, 14.40ms, 18.32ms|6,389 / 59.19ms, 61.05ms, 53.10ms|
|500|20,618 / 984.70ms, 984.14ms, 984.68ms|5,141 / 528.57ms, 4.20s, 308.19ms|6,537 / 176.33ms, 163.20ms, 140.14ms|
|1000|20,136 / 1.01s, 1.01s, 1.01s|3,910 / 1.50s, 3.28s, 642.57ms|6,334 / 210.40ms, 252.86ms, 247.74ms|

p99 값은 각 3회 원본을 순서대로 표시했다. wrk 지연 측정 특성상 완료되지 않은 요청은 빠지며 성공/실패의 체감 지연을 이 숫자만으로 판단할 수 없다.

| 브랜치/연결 | 제출 성공 | 조회 A 성공 | 조회 B 성공 | 비정상 HTTP/본문 응답 | 소켓 오류 (3회 각각) |
|---|--:|--:|--:|--:|---|
|jaeseong / 1|96,067|48,035|48,034|0|none; none; none|
|jaeseong / 10|608,373|304,185|304,184|0|none; none; none|
|jaeseong / 100|963,049|481,529|481,531|0|none; none; none|
|jaeseong / 500|885,550|450,064|450,055|14,068|none; none; none|
|jaeseong / 1000|844,468|449,991|449,994|54,290|none; none; none|
|chan / 1|89,682|44,842|44,842|0|none; none; none|
|chan / 10|472,669|236,336|236,333|0|none; none; none|
|chan / 100|432,416|216,205|216,207|0|none; none; none|
|chan / 500|254,599|127,295|127,293|20|connect 0, read 64, write 0, timeout 550; connect 0, read 21, write 0, timeout 80; connect 0, read 1, write 0, timeout 277|
|chan / 1000|191,787|95,891|95,896|19|connect 0, read 73, write 0, timeout 41; connect 0, read 55, write 0, timeout 25; connect 0, read 24, write 0, timeout 300|
|yyj / 1|35,719|17,861|17,861|0|none; none; none|
|yyj / 10|242,946|121,472|121,473|0|none; none; none|
|yyj / 100|289,983|144,985|144,978|0|none; none; none|
|yyj / 500|293,966|146,957|146,974|0|none; none; none|
|yyj / 1000|281,986|141,080|141,079|244|none; none; none|

비정상 응답은 wrk Lua가 상태·응답 내용·백엔드 식별자를 함께 검사해 실패한 수다. 이 측정의 원본은 HTTP 상태별 분류를 저장하지 않는다. 각 반복의 wrk 완료 요청 수·전송량·소켓 connect/read/write/timeout·CPU/메모리/FD 표본은 wrk-proxy-results.json에 보존했다. 프로세스 RSS/FD 합산은 공유분이 중복될 수 있다.

## 별도 안정성 검증

각 브랜치에서 make test와 make verify를 독립 실행했다. 자체 시험 내용이 다르므로 통과 개수를 같은 점수로 합산하지 않는다.
- jaeseong: make test exit 0, make verify exit 0. 원문 로그 별도 보존.
- chan: make test exit 0, make verify exit 0. 원문 로그 별도 보존.
- yyj: make test exit 0, make verify exit 2. 원문 로그 별도 보존.

YYJ make verify는 Iteration 4의 truncate 검사에서 실패했다. 검증은 HTTP/1.1 200으로 시작하는 불완전 본문을 기대했지만 실제 wire는 HTTP/1.1 502였다. 이 실패를 덮거나 기대값을 바꾸지 않았다. 이후 verify-edge 등 미도달 대상은 미검증이다.

공통 반복 장애: 각 서버 4작업자, 별도 C 백엔드 3개, 부하 측정 종료 후 실행.
- jaeseong: binary 1KiB 제출 3/3; 조회 분산 {'query-a': 5, 'query-b': 5}; 연결 거절 502 10/10; 무응답 504 3/3; 중단 wire {('HTTP/1.1 200', 1): 10}; 복구 201 3/3; RST 30회 후 hello 200; FD 합계 15→15; SIGTERM exit 0.
- chan: binary 1KiB 제출 3/3; 조회 분산 {'query-a': 5, 'query-b': 5}; 연결 거절 502 10/10; 무응답 504 3/3; 중단 wire {('HTTP/1.1 200', 1): 10}; 복구 201 3/3; RST 30회 후 hello 200; FD 합계 6→6; SIGTERM exit 0.
- yyj: binary 1KiB 제출 3/3; 조회 분산 {'query-a': 5, 'query-b': 5}; 연결 거절 502 10/10; 무응답 504 3/3; 중단 wire {('HTTP/1.1 502', 1): 10}; 복구 201 3/3; RST 30회 후 hello 200; FD 합계 42→42; SIGTERM exit 0.

느린 조회 1건과 hello 4건을 함께 보냈으며 모두 정상 응답을 받았다. 이 시험은 완료 순서나 개별 시간을 기록하지 않아 느린 요청에 의한 차단이 없었다는 판정에는 사용할 수 없다. truncate에서 HTTP 상태 줄 하나만 관찰했지만, 클라이언트에서 실제 본문 일부를 읽은 다음 중단시키는 통제 시험은 수행하지 않았다. YYJ는 대기 중이 아닌 worker 2개를 차례로 SIGKILL 후 각각 hello 200·제출 201, 자식 4→3→2를 확인했다. 스레드 모델의 단일 스레드 SIGKILL은 N/A. 자동 재생성이나 공유 잠금 보유 중 사망은 이 시험에 포함되지 않는다.

미검증: mutex 잠금 단계별 사망/복구 재중단·공유 상태 손상, 응답 시작 이후 통제 중단, 느린 요청 완료 순서, 실제 운영 권한, 장시간 누수·TSan, worker 사망 중 CGI 후손, 과부하 회복, 실측 지속 시간 외 1·2·8 worker, 10000 연결. 단일 FD 전후 스냅샷만으로 누수 없음이나 전체 안정성을 보증할 수 없다. 추가 성능 측정은 조건·승인 후 진행한다.

원본: wrk-proxy-results.json, stability-repeat.json, own-tests.json 및 브랜치별 test/verify 로그. 재현 스크립트 wrk-proxy.py, proxy-mix.lua, stability-repeat.py는 /tmp/iter5-compare-20261125 격리 환경과 해당 빌드를 전제로 한다.
