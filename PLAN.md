# 웹 서버 프로젝트: 사용자 선택 중심의 6개 Iteration

## 현재 적용 계획 — 사용자 수정 (2026-11-24)

이 절은 아래 과거 진행 기록보다 우선하는 현재 개인 구현 계획이다. 팀 전체의 채택 결정이나 GitHub Issue 수정으로 간주하지 않는다.

- 기준 코드: Iteration4/Proxy-YYJ, 0b34e68. 현재 팀 코드는 public 고정 루트와 /cafe·/menu·/assets/ 정적 제공, /greet HTML 및 CGI 동적 처리, body_provider 구조를 사용한다. 과거 개인 구현의 경로·정책을 자동 복원하지 않는다.
- Iteration 4: C/POSIX 순차 Reverse Proxy. POST /api/submissions는 제출 백엔드, GET /api/submission-status는 조회 A/B 중 한 곳으로 전달하고 백엔드 응답을 중계한다. 기존 정적·동적 기능과 Issue #4 필수 중계 테스트를 유지한다. 순차 처리에서 여러 백엔드로 분배해도 병렬 처리 효과는 제한된다.
- Iteration 5 방향: 여러 작업자 + 작업자별 non-blocking 이벤트 루프. 연결마다 작업자를 생성하지 않으며, 각 작업자가 여러 클라이언트·백엔드 연결을 관리한다.
- 2026-11-25 작업자 구성 확정: A안, 고정 프로세스 풀 + 프로세스별 non-blocking epoll 이벤트 루프. 스레드별 epoll(B)과 blocking 스레드 풀(C)은 비교 후보/대조군이며 개인 구현에 추가하지 않는다. 연결 수락·분배, 공유 순번, 작업자 수·복구·역압력은 별도 선택이다.
- 외부에는 하나의 클라이언트 접속 주소·포트를 제공한다. 작업자별 외부 포트는 요구하지 않는다. 연결 수락·분배 방식은 구현 전에 대안과 비용을 제시하고 선택받는다.
- Iteration 4에서는 연결별 상태·버퍼·FD 소유권을 명확히 하고 변경 가능한 전역 상태 및 불필요한 I/O 결합을 피한다. Iteration 5의 이벤트 루프·작업자 구조를 미리 구현하지 않는다.
- Iteration 5에서는 백엔드 연결·수신·송신 대기가 다른 연결을 막지 않아야 한다. 느린 클라이언트의 버퍼 상한·역압력·타임아웃 정책도 먼저 설명하고 선택받는다. 기존 blocking 호출·body_provider의 대기 및 수명 계약 변경 범위를 재검토한다.
- 동시 연결 10,000개는 검토 목표이며 처리 가능성을 보장하지 않는다. 실제 요청률·본문 크기·백엔드 지연·CPU·메모리·FD 조건을 제시하고 별도 허락 후 측정한다. 동시 연결 수와 초당 처리량을 구분한다.
- 테스트 백엔드는 C 서버를 재사용한다. 프록시 코드와 테스트용 백엔드 코드는 명확히 분리한다. Python은 검증 스크립트에만 사용하며 Python/Flask HTTP 백엔드는 만들지 않는다.
- 로그인·세션·토큰·권한 관리, 캐싱, 여러 프록시 배치는 추가하지 않는다.
- 시간 배분 4/8/6/10/8/4시간은 기존 초기 가정이다. 새 동시성 방향을 이유로 시간·작업자 수·성능 정책을 자동 확정하지 않는다.
- Iteration 4 정책은 아래 추가 확정 기록에 따라 채택·구현했다. 숫자 IPv4, 요청당 새 연결, 클라이언트·백엔드 요청 HTTP/1.1 및 백엔드 응답 HTTP/1.0·1.1, 응답 스트리밍, 합의한 기한·오류·소유권을 사용한다. 현재 P0-2 보완 후 단위·E2E 회귀를 통과했다. Iteration 5 정책·성능 실험은 여전히 별도 선택이다.
- 합의된 흐름 단위로 요구사항·테스트 후보 기록 → 구현 → 실제 코드 순서 리뷰를 진행한다. 컴파일과 공식 검증 실행을 구분하고 성능 실험은 별도 합의한다.

상세 범위: docs/iteration-4/Snoopykid/iteration-4-requirements.md

## 과거 진행 기록

아래 현재 상태·단계별 기록은 작성 당시의 이력이며 현재 구현 상태나 새 설계 승인으로 해석하지 않는다.


## 현재 상태

- 날짜: 2026-11-21.
- 작업 폴더: C:\Users\David\SW-AI-W02-03-TEMPLATE\jungle-week-13-c-web-server. 폴더 이전 후 중복 문서를 병합했다.
- Iteration 1 구현·컴파일 완료. 사용자 직접 기본 동작 확인을 위해 공식 검증 범위 확정·실행은 보류한다. 성능 측정도 미실행이다.
- Iteration 2: 선택한 최소 범위 구현 및 서버·단위 테스트 바이너리 컴파일 완료. Python E2E 구문 확인 완료. 공식 검증·성능 측정은 미실행이며 단계 완료는 미판정이다. 현재 실행 대상은 src/에서 빌드한 ./server다. Iteration 3~6은 시작 전이다.
- 기존 Ubuntu WSL2에 GCC 11.4.0, GNU Make 4.3, libc6-dev를 준비했다. 도구 설치 당시 기존 Postfix의 빈 mydomain 설정으로 패키지 관리 명령은 오류 종료했으나, 필요한 세 패키지는 install ok installed 상태임을 확인했다. 메일 설정은 변경하지 않았다.

## 전체 계획

40시간은 초기 배분이며 설계·구현·검증·체크포인트 리뷰·통합을 포함한다. 독립 작업 75%, 공동 리뷰·통합 25%를 초기 운영 가정으로 삼고 첫 두 체크포인트에서 실제 통합 시간을 검토해 조정한다. 이는 추가 에이전트 실행 지시가 아니다.

| 단계 | 시간 | 구현 목표 | 추천 검증 범위 | 성능 지표 후보 |
|---|---:|---|---|---|
| 1. TCP 기반 HTTP 응답 | 4시간 | 소켓 연결과 고정 HTTP 응답 | 연결 수락, 응답 형식, 반복 접속, 연결 정리, 포트 충돌 | 응답 지연, 성공률 |
| 2. HTTP 요청 처리 | 8시간 | 요청 라인·헤더·GET·POST 본문 처리 | 부분 수신, 경계, 길이, 잘못된 요청 | 요청 크기별 지연, 처리량 |
| 3. 정적 파일 서빙 | 6시간 | 루트 디렉토리 기반 파일 응답 | MIME, 바이너리, 경로, 오류 | 파일 크기별 지연, 전송 처리량 |
| 4. Reverse Proxy | 10시간 | 라우팅과 백엔드 중계 | 라우팅, 본문, 백엔드 장애·타임아웃·연결 종료 | 직접 접속 대비 추가 지연, 처리량, 오류율 |
| 5. 동시성 | 8시간 | 사용자가 선택할 동시성 모델 | 다중 접속, 느린 클라이언트, 상태 격리, 자원 회수 | 동시 연결 수별 처리량·지연·오류율, CPU·메모리 |
| 6. 안정화 | 4시간 | 회귀·장애 대응·최종 품질 | 전체 기능, 메모리 오류·누수, 시그널 종료, 반복 실행 | 장시간 자원 증가, 부하별 변화 |

위 검증과 지표는 후보이며 실행 조건은 단계별로 확정한다. 최종 안정화 시간을 기능 개발로 모두 소진하지 않는다.

## 공통 체크포인트

1. 설계·동작·성능 선택의 대안, 차이, 구현 비용, 추천 이유를 제시하고 사용자가 결정한 후 구현한다. 단순 문법·변수명은 구현자가 처리한다.
2. 구현 중 문법·링크 오류를 컴파일로 확인할 수 있다. 공식 검증은 단계 구현 종료 시 추천 항목·목적·통과 기준·포함 및 제외 범위를 보여주고 사용자 추가 요청을 반영해 확정한 뒤 실행한다.
3. 검증 결과는 통과·실패·미검증·남은 위험을 구분해 보고한다.
4. 성능 측정 전에 지표·단위·도구·요청 종류 및 크기·동시 연결 수·시간·반복·환경을 제시한다. 추가 지표를 반영하고 명시적인 사용자 허락을 받은 범위에서만 측정한다. 조건 변경과 재측정도 다시 확인받는다.
5. 성능 결과에 따른 최적화나 수정은 자동 적용하지 않는다. 미해결 사항과 통합 결과를 검토해 다음 단계 진행을 사용자와 결정한다.

## Iteration 1 확정 결정

| 항목 | 사용자 결정과 이유 |
|---|---|
| 환경·빌드 | 기존 Ubuntu WSL2, Makefile. C11, 경고 옵션, 디버그 정보, -O0. |
| 주소 체계 | IPv4만 지원해 초기 학습·구현 범위를 제한한다. |
| 바인딩·검증 접근 | 0.0.0.0에 바인딩. 이번 공식 검증은 로컬만 대상으로 하며 외부 PC·WSL 네트워크·방화벽 설정은 필요할 때 별도 결정한다. |
| I/O 모델 | 블로킹·순차 처리. 최종 동시성 모델은 이후 비교 실험으로 멀티프로세스·스레드 풀·non-blocking 이벤트 루프 중 결정한다. |
| 연결 상태 | 연결별 버퍼와 상태는 지역 변수로 관리하고 handle_connection 함수로 분리한다. |
| 실행 | ./webserver <port>. 필수 인자는 십진 숫자 형식과 1~65535 범위를 검증한다. |
| 응답 | HTTP/1.1 200 OK, text/plain; charset=utf-8, 본문 Hello, world! 및 LF 줄바꿈. Content-Length: 14. Connection: close. |
| 요청 수신 | 요청 줄·헤더·끝의 CRLFCRLF를 포함해 최대 8192바이트. 정확히 한도에서 끝난 헤더는 수락하고, 한도 안에 헤더가 끝나지 않으면 431 응답을 시도하고 연결을 종료한다. POST 본문 제한과 의미 파싱은 별도다. |
| 임시값 | 고정 8KiB 수신 버퍼, backlog=128, 수신·송신 호출별 5초. 각각 한 곳의 상수로 관리하고 별도 설정 시스템은 만들지 않는다. 표준·최적·최종값이 아니다. |
| 소켓 옵션 | SO_REUSEADDR 사용. |
| 부분 I/O·오류 | recv 부분 수신과 send 부분 송신을 반복 처리. recv의 EOF·EINTR·타임아웃·기타 오류를 구분하고, EINTR는 종료 표시가 없으면 재시도한다. |
| 자원·종료 | 초기화 실패 시 확보 자원 정리·오류 출력·종료. 클라이언트 I/O 실패는 해당 연결만 정리. SIGPIPE 무시와 송신 오류 확인. SIGINT/SIGTERM 핸들러는 종료 표시만 설정하고, 일반 흐름에서 연결·리스너를 정리한다. 진행 중 응답 완료는 보장하지 않는다. |

### 구현 시 명시한 동작

- 연결 소켓은 main이 소유하고 handle_connection은 빌려 사용한다. 정리는 main의 한 경로에서 한 번 수행한다. Linux close는 EINTR여도 재호출하지 않는다.
- 신호 핸들러에 SA_RESTART를 사용하지 않는다. 종료 표시 확인 직후 신호가 오는 경우에도 accept가 무기한 남지 않도록 리스닝 소켓에도 동일한 SO_RCVTIMEO 5초를 적용한다. 유휴 accept의 타임아웃은 오류 종료가 아니라 종료 표시를 재확인하는 계기다. 종료 반응은 스케줄링 영향과 호출별 타임아웃의 영향을 받을 수 있다.
- 헤더 끝 탐색은 이전 recv의 마지막 3바이트부터 이어서 확인한다. 버퍼는 문자열이 아니라 바이트 배열이다.
- recv가 EOF 또는 타임아웃으로 끝나면 불완전한 요청에 응답하지 않고 연결을 닫는다. 파싱과 오류 응답 정책은 2단계에서 확장한다.

### 알려진 제한·후속 결정

- 전체 헤더 수신·전체 응답 완료 기한은 없다. 조금씩 데이터를 보내는 클라이언트가 계속 연결을 점유할 수 있다. 블로킹·순차 모델에서는 다음 연결 처리도 지연된다.
- 본문 없는 요청을 정상 사용 범위로 삼는다. 지금은 CRLFCRLF 경계만 확인하며 메서드·URI·버전·Host·Content-Length 등 의미를 검사하지 않는다. 완성된 HTTP/1.1 구현이 아니다. 헤더가 완성되면 고정 응답을 보낸다.
- 본문·파이프라이닝·과도한 추가 데이터처럼 읽지 않은 데이터가 남은 상태에서 close하면 TCP reset 때문에 응답 전체가 클라이언트에 전달되지 않을 수 있다. 431 응답 전달도 이 영향을 검토해야 한다. 추가 데이터 배출·연결 종료 정책은 검증 결과와 함께 사용자에게 제시해 결정한다.
- keep-alive, 동적 버퍼, 이벤트 루프, 진행 중 응답 완료를 기다리는 서버 종료는 이번 선택에 포함하지 않는다.
- 2단계에서 RFC 9110·9112 및 HTTP 필드 등록부 등을 근거로 헤더 목록·용도·표준상 필수 조건·선택 가능한 구현 범위를 제시한다. 필수 규칙을 임의 선택 사항으로 표현하지 않는다.
- 버퍼·backlog·타임아웃·빌드·동시성 비교는 EXPERIMENTS.md에 등록만 한다. 지금 비교 구현·계측·실행을 하지 않는다.

## 검증·측정 상태

- 컴파일: Ubuntu WSL2에서 make 성공. GCC 11.4.0, C11, -Wall -Wextra -Wpedantic -g -O0, 경고 없음. 서버 실행·공식 기능 검증·성능 측정은 하지 않았다.
- Iteration 1 공식 검증: 사용자 요청으로 범위 확정·실행 보류. 사용자가 기본 동작을 직접 확인한 뒤 VERIFICATION.md 검토를 재개한다. 아직 미실행이다.
- 성능 측정: 후보·조건 검토 및 명시적 허락 전. 미실행이다.
- 다음 단계 진행: 사용자가 Iteration 2의 요구사항·설계 검토 시작을 지시했다. 이것은 1단계 공식 검증 완료나 2단계 자동 검증·성능 측정 허락을 뜻하지 않는다.

## Iteration 2 개인 결정 — POST /echo 응답 타입 (2026-11-20)

- 현재 대상: feat/iteration-2-YYJ. 팀 기준 변경이 아닌 개인 설계 결정이며, 소스 변경·검증 실행은 하지 않았다.
- 사용자 선택: 최소 구현과 보안상 이점이 있는 경우 application/octet-stream 고정을 선호한다고 명시했다. 표준 검토 결과 이 API 계약에 적절하여 응답 타입을 고정하는 것으로 기록한다.
- 계약: 지원하는 HTTP 프레이밍을 제거한 요청 본문 바이트를 변형하지 않고 응답 본문으로 반환한다. 요청 Content-Type을 복사하거나 실제 형식을 추측하지 않는다. 응답 Content-Length는 반환하는 바이트 길이로 계산한다.
- 근거: RFC 7231 section 3.1.1.5는 응답 표현의 타입을 설명하도록 하며 요청 타입의 자동 반영을 요구하지 않는다. /echo는 임의 바이트를 반환하는 API로 정의한다. 과제도 /echo의 타입 반영을 명시적으로 요구하지 않는다.
- 이점과 한계: 요청 타입의 응답 반영 경로와 관련 검증·저장 처리가 줄고, 사용자 입력을 text/html 등으로 선언해 되돌리는 동작을 피한다. 원래 타입 정보는 응답에서 제공하지 않는다. octet-stream만으로 XSS나 MIME sniffing 방지를 보장하지 않는다.
- 경계: Content-Encoding, Transfer-Encoding/chunked, 콘텐츠 협상, 추가 보안 헤더는 별도 결정 사항이다. 이번 결정으로 자동 확정·구현하지 않는다. 정적 파일 및 Reverse Proxy 응답 전체에 이 정책을 적용하지 않는다.
- 표준: https://www.rfc-editor.org/rfc/rfc7231.html#section-3.1.1.5
## Iteration 2 확정 결정

- 본문은 Content-Length 방식만 지원한다. Transfer-Encoding은 검출해 명시적으로 거부·종료한다. chunked 지원은 최종 소스 구현·검증 완료 시 재검토할 항목으로 남긴다.
- POST /echo는 요청 본문을 바이트 그대로 반환한다. 응답 Content-Type은 application/octet-stream으로 고정하고 Content-Length는 반환 본문의 실제 바이트 길이로 계산한다.
- 요청 Content-Type 반영, JSON 파싱·압축 해제는 이번 범위에서 제외한다. 사용자 지정 타입을 응답에 그대로 반영하지 않으며 최소 구현을 유지하기 위한 선택이다.
- 나머지 경로·오류 정책·요청 상한·버퍼 소유권은 선택 전이다. 기존 공식 검증·성능 측정 보류 및 선택 절차는 유지한다.

## Iteration 2 본문 상한 임시 결정 및 프레이밍 검토 (2026-11-20)

- 사용자 요청에 따라 본문 상한은 추천안 64KiB(65,536바이트)를 임시 채택하고, 요청 줄·헤더 상한 8KiB와 별도로 관리한다. 메모리 할당 방식은 이 결정으로 확정하지 않는다.
- RFC는 일반 요청 본문 상한을 64KiB 또는 1MiB로 권고하지 않는다. 초기 기능 범위와 자원 상한을 명확히 하기 위한 프로젝트 정책이며 추후 실험으로 재검토한다.
- 소스·팀 통합 기준은 변경하지 않았다. 선언 길이 상한 초과의 413 응답, 짧은 본문의 오류 코드·전체 완료 기한, 초과 데이터·연결 종료의 구체 정책은 대안 검토 후 사용자와 확정한다.
- Content-Length는 지원 프레이밍에서 본문의 바이트 경계를 정의한다. 부족분을 임의 데이터로 채우지 않는다. EOF·타임아웃 전에 요청처럼 보이는 바이트가 추가로 와도 내용만으로 실제 다음 요청인지 구분할 수 없다.
- 선언 길이 이후의 데이터는 현재 본문에 포함하지 않는다. 지속 연결에서는 다음 요청 데이터일 수 있어 초과 수신 자체를 길이 위반으로 단정하지 않는다. 현재 close 정책과 향후 프록시 프레이밍 일관성을 별도로 검토한다.
- 기능 검증·성능 실험은 문서 후보 등록만 했으며 실행하지 않았다.
## Iteration 2 추가 확정: 응답·크기·본문 메모리 (2026-11-20)

- GET /와 /hello는 모두 새 파서로 검증하고 200 OK·text/plain·줄바꿈 없는 Hello 5바이트로 응답한다.
- 요청 줄·헤더 합계는 8KiB, 본문은 별도 64KiB 임시 상한이다. Content-Length 형식·오버플로우 검사 후 상한과 비교하며, 유효 선언 길이 초과는 할당·전체 수신 전에 413으로 거부·종료한다.
- 본문은 검증된 길이만큼 malloc 한 번, 길이 0이면 할당하지 않는다. 연결 처리 함수가 소유하고 하위 수신·송신 함수는 빌려 사용하며 정상·오류·종료 경로에서 정확히 한 번 free한다. 할당 실패를 확인하고 문자열 대신 바이트 배열로 취급한다.
- 헤더와 동반된 본문을 보존하고 부족분만 수신한다. EOF·타임아웃으로 불완전한 본문을 정상 echo하지 않는다. 선언 길이 이후 추가 데이터는 현재 본문에 넣지 않고 초과 수신만으로 길이 위반이라 단정하지 않는다.
- 연결당 요청 하나와 Connection: close를 유지한다. 중복·충돌 길이의 응답 정책, 전체 헤더·본문 수신 기한과 미수신 데이터가 있는 연결의 종료 방식은 사용자 선택 대기다.
- 64KiB·1MiB 상한 비교, 고정 본문 공간·정확한 길이 한 번 할당 비교는 추후 실험이다. 16GB 노트북의 실제 WSL CPU·메모리 환경과 동일 조건·반복을 기준으로 검토하되 지금 구현·실행하지 않는다.
- 소스·테스트 코드는 아직 변경하지 않았고 공식 검증·성능 측정도 실행하지 않았다.
## Iteration 2 추가 확정: 오류·수신 기한 (2026-11-21)

- 사용자 선택: 오류·중복 길이는 앞서 제시한 기본안으로 엄격히 거절한다. 잘못된/오버플로우 길이, 동일·상충 중복 및 쉼표 목록 길이, Content-Length/Transfer-Encoding 동시 존재, 불완전 본문 EOF는 가능한 경우 400. 본문 상한 초과 413, 헤더 상한 초과 431, 수신 기한 초과 408, 할당 실패 500. 고정 text/plain 오류 본문·정확한 길이·Connection: close를 사용하고 상대 reset·서버 종료 중 무리하게 응답하지 않는다.
- 호출별 수신·송신 5초, 전체 헤더 수신 7초(accept 시 시작), 전체 본문 수신 15초(헤더 수신 완료 시 시작)를 임시값으로 채택했다. 호출별 제한과 전체 기한의 남은 시간 중 더 짧은 대기이며 조각 도착으로 전체 기한을 늘리지 않는다.
- 전체 응답 송신 기한은 별도 결정 사항이다. 제한적 읽기 후 종료의 순차 처리 지연 비용과 Expect의 100 Continue/417 대안은 아직 선택 전이다.
- 결정 기록·검증 후보만 갱신했다. 소스·테스트 코드 및 공식 검증·성능 측정 실행 상태는 변경하지 않았다.
## Iteration 2 추가 확정: 개별 연결 종료·Expect (2026-11-21)

- 사용자 선택 A: 응답 송신 후 shutdown(SHUT_WR), 최대 1초·64KiB 제한적 수신 폐기 후 close. 두 상한은 임시값이다. 다음 요청은 처리하지 않으며 순차 처리 지연 비용과 응답 전달 비보장 한계를 문서화한다.
- 서버 SIGINT/SIGTERM 종료 때는 제한적 폐기를 기다리지 않는다. 개별 연결 정리와 응답 완료를 기다리는 서버 셧다운을 혼동하지 않는다.
- 사용자 선택 A: 정상 Expect: 100-continue 요청을 지원한다. 헤더 검사 후 본문 부족 시 100을 먼저 보내고, 이미 본문 전체 수신 또는 헤더만으로 거부 가능하면 불필요한 100을 보내지 않는다.
- 구문 허용 범위·요청 표현·일부 경로별 오류 정책은 추가 검토 전이다. 소스와 테스트 코드는 아직 변경하지 않았다.
## Iteration 2 최소 파서 범위 확정 및 구현 착수 (2026-11-21)

- HTTP/1.1·origin-form 제한, 쿼리 앞 원래 경로 라우팅, percent 구문 검사만 수행하며 디코딩·정규화는 하지 않는다. 필드는 원본 헤더의 포인터+길이로 빌려 참조한다.
- 길이 정보 없는 /echo는 0바이트 200 응답이다. 명시적 길이는 정확한 수신 완료를 요구한다. 추가 데이터를 본문으로 추측하지 않는다.
- 사용자 채택 기본 구문·안전·명시적 거부 규칙과 각 성공/실패 후보를 개인 문서에 먼저 누적했다. 광범위한 조합·장애·장시간 회귀는 6단계에서 확대한다.
- 구현 완료 후 main()부터 실제 흐름을 따라 대화의 선택·이유를 연결해 같은 작업에서 설명한다. 별도 완료 시점 자동화는 만들지 않는다.
- 현재는 구현 착수 상태이며 공식 검증·성능 측정은 허락 전이다.
## Iteration 2 구현 결과와 아직 남은 검증 (2026-11-21)

- src/http.h·http.c: 원본 바이트를 빌린 요청 표현, HTTP/1.1 origin-form 파싱, URI·Host·길이·필드·미지원 처리, 경로별 판정.
- src/server.c·main.c: 수락 시각·종료 표시 전달, 헤더 끝/전체 수신량 분리, 본문 한 번 할당·한 번 해제, 동반·분할 본문, 100 Continue, 실제 길이 응답, 5/7/15초 기한, 1초·64KiB 단계적 정리 구현. 기본 소켓 I/O·FD 소유권은 유지한다.
- Makefile: 기존 일반/단위 테스트 빌드 옵션을 유지하면서 http.c와 기본 파서 테스트를 연결했다. make verify는 1단계 회귀와 기본 2단계 E2E를 실행하는 명령이다. 이 작업에서 명령을 실행하지는 않았다.
- 1단계 E2E의 정상 요청에 Host를 보완하고 스트림 응답을 누적 수신하도록 수정했다. 상한 초과는 새 선택인 431 기준이다. 2단계 E2E는 기본 경로·빈/바이너리/분할/추가 데이터 본문·선택 상한 경계·기본 오류·100 Continue를 다룬다.
- 실제 실행한 확인: 서버 및 네 단위 테스트 바이너리 컴파일 성공, 두 E2E Python 파일 구문 컴파일 성공. 서버 프로세스·테스트 바이너리·curl·E2E는 실행하지 않았다. sanitizer 바이너리 컴파일은 메모리 검사 통과가 아니다.
- 남은 기본 검증: 전체 기한·호출별 제한, 폐기 기한/바이트 상한, 할당 실패와 종료 중 정리의 제어된 재현 방법·실행 범위를 사용자와 확정해야 한다. 광범위한 조합·경합·장시간 회귀는 6단계에서 확대한다. 성능·비교 실험은 별도 허락 대상이다.
- 요청한 설명은 docs/iteration-2/Snoopykid/iteration-2-walkthrough.md에 main()부터 실제 코드와 대화의 선택 이유를 연결해 작성했다. README 실행 안내도 현재 ./server 기준으로 갱신했다.
- 위쪽 Iteration 1의 ./webserver·14바이트·-O0 기록은 초기 개인 선택 이력이다. 현재 팀 기반 ./server·5바이트·일반 -O2와 혼동하지 않는다.

## 2026-11-21 경로·구조 변경
이전 ./server 및 src/*.o 기록은 당시 경로다. 현재는 ./build/server, build/obj/*.o, build/test_*를 사용한다. 라우팅과 응답 내용 선택은 src/router.c로 분리했으며 현재 프로토콜·오류·본문 정책은 유지한다. 기존 바이너리는 Git 추적에서 제외한다. 이번 변경의 확인은 컴파일·링크와 산출물/Git 경로 검사까지이며 공식 기능 검증·성능 측정은 별도 확정 전까지 보류한다.
### 라우터 분리·산출물 통합 확인 결과
WSL에서 make all test_parse_port test_recv_header test_send_all test_http_parser를 실행해 서버(-O2)와 네 개 테스트(sanitizer 옵션)의 컴파일·링크에 성공했다. 테스트 바이너리는 실행하지 않았다. Python AST 구문 확인 성공. make -n test verify는 새 경로를 사용하며 실제 검증은 실행하지 않았다. Git diff --check는 공백 오류 없이 통과했고 LF/CRLF 경고만 있었다.
build/server·build/obj/main.o·build/test_send_all·build/legacy/webserver가 무시됨을 확인했다. server 및 세 기존 테스트 바이너리를 인덱스에서 제거했으며, 알려진 산출물은 더 이상 추적되지 않는다. 프로젝트 루트·src·tests/unit·scripts에서 ELF/객체 파일이 남아 있지 않음을 확인했다. 기존 webserver는 build/legacy에 보관했다. 커밋·푸시하지 않았고 다른 기존 staged 변경은 유지했다.
공식 기능 검증(라우팅/Allow·Hello/echo·오류 회귀), 반복·병렬/clean 후 재빌드 검증 및 성능 측정은 미실행이다.
## Iteration 4 확정 추가 및 분배 한계 (2026-11-24)

- 실행 설정은 ./build/server <port> <proxy.conf> 파일 방식이다. public 정적 루트는 유지한다.
- GET /api/hello는 조회 A/B로 분배하며 원래 경로·쿼리를 보존한다.
- Iteration 4는 Round-Robin을 사용한다. 선택 시 순번을 넘기고 실패 시 다른 백엔드로 자동 재전송하지 않는 추천안을 기준으로 한다. 제출 POST도 자동 재시도하지 않는다.
- Iteration 5의 진행 중 요청 수 기준 선택과 동률 시 Round-Robin은 검토 후보이며 최종 채택은 아니다. 응답 시간 기반 자동 전환도 현재 확정 범위가 아니다.
- 반례: 조회 A가 매우 무거운 요청 1개를 처리하고 B가 가벼운 요청 2개를 처리하면 요청 수만 비교하는 방식은 A를 선택할 수 있다. 진행 중 요청 수는 실제 CPU·메모리·요청 비용을 보장하지 않는다.
- 후속 검토 후보는 응답 시간·미완료 요청의 경과 시간·백엔드 제공 부하·알려진 요청 비용과 용량 가중치, 백엔드별 동시 요청/대기열 상한이다. 관측 지연·공유 상태·재확인·오탐·구현 및 검증 비용을 비교하고 선택받는다. 타임아웃만으로 백엔드 연산 중단을 보장하지 않는다.
- 기본 검증 후보에 무거운 요청 1개 대 가벼운 요청 여러 개의 분배 반례를 등록한다. Iteration 5 검토 항목이며 이번에 계측·장애 자동 우회·부하 실험을 구현하거나 실행하지 않는다.

## Iteration 4 첫 함수 흐름 구현 결과 (2026-11-24)

- src/proxy.h·proxy.c: 숫자 IPv4 설정 로더, 프록시 목적지 판별, 호출자 소유 상태의 Round-Robin 선택 구현.
- proxy.conf.example: submission/query-a/query-b 주소 설정 예시. 런타임 main 통합 전이므로 아직 CLI에서 설정 파일을 읽지 않는다.
- tests/unit/test_proxy_selection.c: 정상/잘못된 설정, 실패 시 출력 보존, 메서드·경로 판별, 조회 순번과 독립 상태의 테스트 소스 추가.
- Makefile: 새 모듈과 테스트를 build/ 아래 컴파일·링크한다.
- 확인: WSL에서 make all test_parse_port test_recv_header test_send_all test_http_parser test_body_send test_proxy_selection 성공. 서버 -O2 및 6개 단위 테스트 ASan/UBSan 컴파일·링크 성공이며 바이너리는 실행하지 않았다.
- git diff --check는 공백 오류 없이 통과했다. 줄바꿈 변환 경고만 있었다.
- 기존 main·요청 처리·정적/동적 코드의 런타임 동작은 변경하지 않았다. 아직 실제 프록시 중계는 하지 않는다.
- 남은 연결 기한·오류 매핑·구체 FD 소유권을 확정한 뒤 main → 설정 → 요청 처리 → 백엔드 연결·중계 순으로 통합한다. 공식 검증·성능 측정·커밋·푸시는 하지 않았다.

## Iteration 4 중계 통합 결과 (2026-11-24)

- 이전 모듈 단독 단계 이후 main·handle_client·실제 중계까지 통합했다. 현재 실행은 ./build/server <port> <proxy.conf>이며 사용자가 이동한 build/proxy.conf.example을 지정한다.
- 기존 정적·동적 라우트는 유지하고, 제출 1·조회 A/B 분배, 원래 경로·쿼리, HTTP/1.1 전용 중계, 헤더 필터·Via·Host 변경, 고정/청크/연결 종료 본문, 오류·기한·정리를 구현했다.
- 승인된 임시 기한과 500/502/504 및 구체 소유권을 적용했다. 실제 요청/응답 메모리·FD 회수는 아직 공식 실행 검증 전이다.
- src/socket_io.c/h로 기존 공통 I/O를 분리했다. tests/backends/server.c는 이 공통 I/O와 HTTP 파서만 링크하며 프록시 코드 없이 별도 바이너리로 빌드한다.
- main/handle_client를 상위에 배치하고 역할·길이를 드러내는 이름으로 정리했다. 실제 코드 리뷰는 iteration-4-walkthrough.md에 있다.
- 서버·C 백엔드·7개 ASan/UBSan 단위 테스트의 컴파일·링크 성공. 6개 Python 보조/검증 파일 구문 컴파일 성공. 공식 테스트·서버/백엔드 실행·성능 측정은 하지 않았다.
- make -n verify clean으로 예정 명령만 확인했다. clean은 사용자 설정 예시를 삭제하지 않는다. build/server·build/test_backend·build/proxy.conf.example의 Git 무시를 확인했다.
- 공식 검증 후보는 단위 7개 + E2E 5개다. 검증 실행은 사용자 범위 확정 전이며 성능 실험은 별도 허락 대상이다. 커밋·푸시하지 않았다.

## 핸드오프 검토 후 확정 — P0-2만 변경 (2026-11-25)

기준 커밋: f32e17d, 작업 브랜치 Iteration4/Proxy-YYJ. 기존 결정과 새 변경을 구분한다.

| 항목 | 확정 상태 | 처리 |
| --- | --- | --- |
| P0-1 Host | 기존 결정 유지 | 선택한 백엔드 IPv4:port로 재작성한다. 원본 Host를 요구하는 C2는 현재 계약과의 차이다. 구체적인 팀 요구가 생기면 변경안으로 검토한다. |
| P0-2 Connection 보호 필드 | 새 오류 정책 채택 | 유효하게 파싱·라우팅된 프록시 요청에서 Host, Content-Length, Transfer-Encoding을 Connection 옵션이 지명하면 400으로 거절한다. 본문 할당·추가 수신, 100 Continue, 백엔드 선택·연결 전에 판정한다. |
| P0-3 CLI | 기존 결정 유지 | 설정 파일 인자가 필수다. 설정 없는 실행, 내장 기본 백엔드, 기존 회귀 스크립트의 CLI 변경 원복은 추가하지 않는다. C14의 단독 포트 실행 기대는 현재 계약과의 차이다. |
| HTTP/1.0 | 기존 결정 유지 | HTTP/1.1 전용이다. C7은 지원 범위 차이이며 자동 수용하지 않는다. |
| 타임아웃·소유권·분배 | 기존 결정 유지 | 이미 확정한 임시값·수명·Round-Robin을 변경하지 않는다. |

프록시의 기존 구문/길이 파싱과 메서드 오류 우선순위는 유지한다. 그 뒤 보호 필드 검사가 통과해야 프록시 경로를 확정한다. 로컬 경로는 이 새 오류 정책을 적용하지 않는다. Connection 옵션은 전체 토큰 기준·대소문자 무시로 검사하고, 모든 Connection 줄을 순회한다. 일반 지명 헤더 제거는 기존대로다.

여기서 본문 수신 전은 본문 전용 할당·추가 수신 단계 이전을 뜻한다. 헤더 recv에 동반 도착한 바이트나 종료 시 기존 제한적 수신 폐기까지 없어진다고 주장하지 않는다.

P1/P2도 기존 .md 확정 기록을 먼저 확인한다. 외부 교차 테스트와 정책 차이를 구현 결함으로 일괄 분류하지 않고 실제 변경이 필요한 사항만 설명·합의한다. main 머지·커밋·푸시·성능 재측정은 하지 않는다.

## P0-2 보완 검증 완료 (2026-11-25)

- 기존 Host 재작성·설정 파일 필수·HTTP/1.1 전용·기한·소유권·분배 정책 유지. C2/C14/C7은 현재 확정 계약과의 차이로 분리한다.
- 새 Connection 보호 필드 조기 400만 구현했다. 본문 전용 처리·100 Continue·백엔드 접속 전 차단이며 일반 지명 헤더·로컬 경로·기존 오류 우선순위를 유지한다.
- make clean 및 make all 성공. make test 단위 7개 ASan/UBSan 통과, make verify E2E 5개 통과. 최종 실패/skip 없음. 성능·외부 교차 스위트 전체는 미측정/미실행.
- 새 E2E는 첫 응답 400·100 미송신·백엔드 접속 0건과 정상 회귀를 확인했다. 상세 결과: docs/iteration-4/Snoopykid/iteration-4-p02-verification-results.md.
- 사용자 설정 예시는 clean·빌드 전후 해시 동일. main 머지·커밋·푸시는 하지 않았다. 이후 제안도 .md의 기존 결정과 실제 변경을 구분한다.

## P1-4 변경 승인 — 백엔드 HTTP/1.0 응답 수용 (2026-11-25)

사용자가 백엔드 HTTP/1.0 수용·클라이언트 HTTP/1.1 승격 범위의 진행을 승인했다. 이 기록은 이전 HTTP/1.1 전용 결정 중 백엔드 응답 범위만 변경한다. 과거 C7의 502는 당시 계약에 맞는 결과였으며 과거 기록을 구현 결함으로 소급 변경하지 않는다.

- 클라이언트 요청과 백엔드로 전달하는 요청은 HTTP/1.1을 유지한다.
- 백엔드 응답은 HTTP/1.0·HTTP/1.1만 수용하고 클라이언트 상태 줄은 HTTP/1.1로 구성한다. 상태 코드와 상태 문구는 그대로 보존한다.
- HTTP/1.0의 Content-Length 응답은 정확한 바이트 수를 중계한다. 길이 없는 정상 EOF 응답은 기존 chunked 전송기를 재사용한다.
- HTTP/1.0에서 1xx 또는 Transfer-Encoding을 사용한 응답은 잘못된/지원 밖 조합으로 502 거절한다. 중복 CL·CL+TE·잘못된 버전·조기 EOF 및 기존 204/304 검증은 유지한다.
- 연결·버퍼·FD 소유권, 헤더 상한, 16KiB 스트림, Host 재작성, 필수 설정 파일, 타임아웃·Round-Robin·P0-2 보호 헤더 검사는 변경하지 않는다.
- GET의 CL: 0 합성, HTTP/1.1 1xx의 Connection: close, 모듈 분리·메모리 수명 변경 등 다른 P1/P2 제안은 이번 작업에 포함하지 않는다.

## P1-4 HTTP/1.0 응답 수용 완료 (2026-11-25)

- 사용자 승인으로 백엔드 HTTP/1.0 응답을 추가 수용하고 클라이언트 상태 줄은 HTTP/1.1로 구성한다. 상태 코드·문구·본문은 보존한다. 클라이언트와 백엔드 요청은 기존 HTTP/1.1이다.
- 고정 길이/EOF·204/304·커스텀 문구·조기 종료 및 1.0의 잘못된 1xx/TE를 단위·C 백엔드 E2E로 검증했다.
- make clean·make all 성공. ASan/UBSan 단위 7개와 E2E 5개 통과, 실패/skip 없음.
- P0-2, Host·CLI·타임아웃·소유권은 유지한다. 다른 P1/P2 보완은 이번 작업에 포함하지 않았다.
- 성능·부하·외부 교차 스위트 전체는 재실행하지 않았고 머지·커밋·푸시하지 않았다.
- 상세 기록: docs/iteration-4/Snoopykid/iteration-4-http10-verification-results.md. 이전 HTTP/1.1 전용 및 C7 계약 차이 기록은 변경 이전 이력이다.

## P1-6 진행 — 요청 길이와 중간 응답 헤더 생성 (2026-11-25)

사용자의 다음 작업 진행 지시에 따라 핸드오프 P1-6의 두 항목만 보완한다.

- 본문도 원래 Content-Length도 없는 GET에는 Content-Length: 0을 합성하지 않는다. 명시한 CL=0 및 본문이 있는 GET은 정확한 길이를 유지한다.
- POST는 원래 길이 필드가 없어도 기존 빈 본문 계약에 맞춰 Content-Length: 0을 생성한다. 본문이 있는 요청의 바이트 경계는 그대로다.
- HTTP/1.1 1xx 중간 응답에는 Connection: close를 생성하지 않는다. 기존 hop-by-hop 제거와 Via, 상태·전달 헤더는 유지한다. 최종 응답에는 Connection: close를 유지한다.
- main → handle_client → forward_request_to_backend의 길이 필드 생성 조건과 receive_backend_response_headers → build_client_response_headers의 close 생성 조건만 바꾼다.
- HTTP/1.0 백엔드 수용, P0-2, Host 재작성, 설정 필수·기한·FD/버퍼 소유권·분배·청크/트레일러는 유지한다. 파일 분리·메모리 수명 변경·성능 최적화는 이번 범위가 아니다.

## P1-6 헤더 보완 완료 (2026-11-25)

- 본문·원래 CL이 없는 GET의 CL: 0 합성을 생략하고 명시적 길이·POST 길이 정책은 유지했다.
- 중간 1xx에는 close를 생성하지 않으며 Via·전달 헤더·최종 응답의 close는 유지했다.
- clean·빌드 성공, ASan/UBSan 단위 7개 및 E2E 5개 통과. 최종 실패/skip 없음.
- 실제 socketpair 송신과 C 백엔드 수신, 103/최종 200 분리 검사 및 기존 P0-2·HTTP/1.0·기한·FD 회귀를 확인했다.
- 성능·외부 교차 스위트 전체는 재측정하지 않았고 머지·커밋·푸시하지 않았다. 다른 구조·메모리 보완은 포함하지 않았다.
- 상세 결과: docs/iteration-4/Snoopykid/iteration-4-p16-verification-results.md. 이전 P1-4 보고서의 P1-6 미변경 설명은 당시 이력이다.

## P1-7 진행 — 에지 스위트 흡수 (2026-11-25)

- 참고 eda192c:scripts/verify_edge_cases.py의 34개 검증 아이디어를 현재 계약으로 이식한다. Python mock/RawBackend, 선택적 CLI, 3초 헤더·1초 본문 idle 기대값은 가져오지 않는다.
- 기존 C 백엔드 fixture와 Iteration 4 프로세스·요청 helper를 재사용한다. 느린 본문·지연/부분/slow-drip 헤더·요청 수신 알림은 테스트 C 코드에서만 제공한다.
- E01~E34 기본 사례와 C2/C3/C7/C12/C13/C14 대응 검증 6개를 make verify-edge로 제공하고 make verify에도 포함한다.
- C2는 기존 Host 재작성 계약, C14는 필수 설정 계약을 검증한다. 외부 원본 기대값 차이를 현재 구현 결함으로 분류하거나 외부 교차 스위트 전체 통과로 표기하지 않는다.
- 지연 헤더 2초는 현재 1초 기한에 따라 504다. 본문 idle은 5초를 유지한다. 느리지만 진행하는 본문은 현재 idle/전체 기한 안에서 정상 완료해야 한다.
- C12는 0.4초 간격 C 백엔드 헤더가 전체 1초를 연장하지 않는지 확인한다. C13은 C 백엔드 수신 알림을 확인한 후 SIGTERM으로 종료해 헤더 timeout을 기다리지 않는지 검증한다.
- 시간은 기능 기한·실패 판정에만 사용한다. 요청별 지연 통계·처리량·CPU/RSS 등 성능 지표는 수집하지 않는다.
- 제품 src 코드는 이번 범위에서 변경하지 않는다. 파일 분리·메모리/정책 최적화도 추가하지 않는다. 테스트에서 새로운 제품 문제가 발견되면 원인과 수정안을 별도 보고한다.
## P1-7 엣지 검증 통합 완료 (2026-11-25)

- make verify-edge를 추가하고 make verify에도 포함했다. 테스트용 C 백엔드를 재사용하며 제품 src/ 17개 파일은 이번 작업 전후 해시가 같다.
- clean·컴파일/링크 성공, ASan/UBSan 단위 7개와 E2E 스크립트 6개 성공. 신규 E01~E34 및 C2/C3/C7/C12/C13/C14 대응 6개, 총 40개 통과·실패 0·skip 0.
- Host 재작성·설정 필수 및 기존 기한을 유지해 계약 차이를 분리했다. 기능 기한만 검증했고 성능 측정은 하지 않았다.
- 상세 범위·한계·로그: docs/iteration-4/Snoopykid/iteration-4-p17-verification-results.md. 머지·커밋·푸시는 수행하지 않았다.
## P2-B 구현 경계 및 검증 기준 (2026-11-25)

사용자가 파일 분리와 메모리 수명 개선을 함께 하는 B안을 선택했다.

흐름: main → handle_client → proxy_prepare_response → connect_backend → forward_request_to_backend → prepare_backend_response_headers → send_prepared_response → read_backend_body_chunk → cleanup_backend_response.

- proxy_transfer.c는 준비·실패 정리를 조율한다. proxy_io.c는 연결/기한/수신, proxy_headers.c는 요청 송신·응답 헤더 준비, proxy_body.c는 본문/청크/트레일러를 맡는다. 내부 구조와 모듈 간 함수는 proxy_internal.h로 제한한다.
- 스트림에는 기존 16KiB 수신 배열을 유지한다. raw_header 8KiB와 재구성 client_header 16,640바이트는 헤더 준비의 지역 작업 공간으로 옮긴다. 지역 공간도 메모리를 사용하며, 향후 작업자 스택 예산 검토가 필요하다.
- 최종/중간 응답을 포함한 송신 헤더는 완성 길이만 한 번 할당·복사한다. provider가 소유하고 첫 next 호출(송신 함수가 헤더 송신 완료 후 호출)에서 해제한다. 본문 없는 경우나 헤더 송신 실패로 next가 호출되지 않으면 cleanup이 해제한다. wire_header 참조는 첫 next/cleanup까지 유효하다.
- 최종 chunked 응답의 Connection 필드만 원문 필드 줄로 별도 보존해 헤더 작업 공간 종료 후에도 기존 트레일러 거절 정책을 유지한다. 필드 없는 경우 할당하지 않는다. 응답 헤더 전체나 파싱 slice를 지역 공간 밖에 빌려 두지 않는다.
- 트레일러는 기존 8KiB 상한의 지역 공간에서 검사 후, 비어 있지 않을 때 정확한 길이만 할당한다. 본문 전 할당 실패는 기존 500, 본문 처리 중 실패는 BODY_ERROR로 연결을 닫고 새 오류 응답/종료 청크를 붙이지 않는다.
- 백엔드 FD는 준비 중 조율 함수, 성공 후 provider가 소유한다. client FD·요청 본문 소유권, Host 재작성·필수 설정·HTTP 버전·라우팅·타임아웃·상한은 유지한다. 이벤트 루프·동시성·새 콜백·성능 측정은 추가하지 않는다.

검증 후보/통과 기준:
- 기존 make test(ASan/UBSan), make verify 6개 스크립트/edge 40개 회귀 통과.
- 헤더 작업 공간 종료 후 상태/본문/Connection 지명 정보 정상 사용; 다중 Connection·대소문자·지명 트레일러 거절 유지.
- 헤더 송신 후 첫 next에서 출력 헤더 회수; 본문 없음/헤더 송신 실패의 cleanup 경로 회수.
- 준비 단계 state/Connection/출력 헤더 및 본문 단계 트레일러 할당 실패 주입, FD 정리와 할당 수명 확인.
- chunked·EOF·CL·1xx·204/304·HTTP/1.0·바이너리·불완전 응답 회귀, 오류 후 서버 생존/FD 확인.
- 컴파일·링크와 실행 검증은 별도로 보고한다. 성능·RSS·1만 연결 실험은 제외한다.

## P2-B 완료 (2026-11-25)

- proxy_transfer 조율 + proxy_io/proxy_headers/proxy_body와 내부 헤더로 분리했다. raw_header·재구성 공간은 준비 단계 지역 공간, 송신 헤더·최종 chunked Connection 필드·실제 트레일러는 필요한 길이만 소유한다.
- 첫 body next에서 송신 헤더를 회수하고, 그 전에 끝나거나 실패하면 cleanup이 회수한다. 트레일러 검사에 필요한 지명 정보는 복사해 지역 헤더를 참조하지 않는다. 기존 동작·기한·FD 책임은 유지한다.
- clean·컴파일/링크 성공. ASan/UBSan 단위 8개와 E2E 스크립트 6개 통과, 엣지 40개 통과·실패 0·skip 0. 수명 테스트 추가 보강도 통과했다.
- 제품에는 새 가변 전역/주입 훅을 추가하지 않았다. 메모리 수치는 코드상 공간 설명만 기록하며 CPU/RSS/처리량·성능은 측정하지 않았다. 동시성/라우팅 확장·머지·커밋·푸시는 포함하지 않았다.
- 실제 흐름: docs/iteration-4/Snoopykid/iteration-4-walkthrough.md. 상세 결과/한계/로그: docs/iteration-4/Snoopykid/iteration-4-p2-verification-results.md.
## P2-9 A안 — 라우팅 판별 일원화 구현 전 기준 (2026-11-25)

사용자가 A안을 선택했다. 로컬/프록시의 경로·메서드는 router.c의 단일 routes[]에서 판별한다. IPv6·가변 백엔드·정책 콜백·설정 변경·동시성은 추가하지 않는다.

흐름: main → handle_client → http_parse_request → http_select_route → 프록시인 경우 proxy_validate_request → 본문 할당/100 Continue/수신 → 기존 로컬 handler 또는 proxy_select_backend/proxy_prepare_response → 송신/정리.

- http_select_route는 선택 결과에 로컬 handler 또는 프록시 목적지 역할을 담는다. 라우팅 역할 enum은 router.h로 이동하고 proxy.h는 이를 사용한다. 기존 http_select_handler/proxy_match_request의 중복 판별 진입점은 제거한다. 테스트도 새 진입점을 사용한다.
- proxy_validate_request는 기존 보호 Connection 필드 검사만 담당한다. 경로·메서드를 다시 검사하거나 I/O·할당·선택 순번 변경을 하지 않는다. server가 라우팅 성공한 프록시 요청에만 호출한다.
- 기존 정확 일치/접두사와 최장 일치, GET/POST 외 501, 미등록 GET/POST 404, 등록 경로의 잘못된 GET/POST 405·Allow, 이후 보호 필드 400 순서를 유지한다. 보호 필드가 있어도 로컬 요청에 프록시 검사를 적용하지 않는다.
- 헤더 파서의 오류/413/431은 라우터보다 먼저 결정한다. 본문·100·백엔드 접속보다 라우팅/보호 검사가 앞선다. A/B 순번은 본문 완료 후 기존 선택 지점에서만 바뀐다.

검증 기준:
- 전체 로컬 8개/프록시 3개 경로의 목적지·메서드·Allow, 정확 경로와 접두사 경계, /api/hello/·/api/submissions-extra·미등록 경로, 미지원 메서드 501을 표 기반 단위 테스트로 확인한다.
- 실패 결과는 handler=NULL/LOCAL로 초기화하고 입력 target·headers 및 선택 순번은 변경하지 않는다. 검사만으로 handler를 실행하거나 메모리·FD를 확보하지 않는다.
- 보호 헤더와 잘못된 메서드·미등록 경로 조합의 405/501/404 우선순위, 정상 로컬 경로 통과, 정상 프록시의 조기 400/100 미송신/백엔드 접속0 회귀를 확인한다.
- clean·컴파일/링크와 make test(ASan/UBSan), make verify(6개 E2E/edge40)를 구분해 보고한다. 성능 측정·머지·커밋·푸시는 제외한다.

## P2-9 A안 완료 — 라우팅 일원화 (2026-11-25)

- 로컬 8개·프록시 3개 경로/메서드를 router.c 단일 routes[]와 http_select_route로 모았다. 결과는 request_route의 local_handler/proxy_route다.
- handle_client에서 파서 → 라우터 → 프록시만 proxy_validate_request → 본문/100 → 기존 로컬 실행/백엔드 선택·중계 순서를 유지한다. 이전 중복 판별 함수는 제거했다.
- 오류/Allow·보호 검사 범위·선택 순번·입력·소유권·기한·버퍼 수명을 유지했다. IPv6·가변 백엔드·정책 콜백·동시성은 추가하지 않았다.
- clean·컴파일/링크 성공, ASan/UBSan 단위 8개와 E2E 스크립트 6개 통과. edge40 통과·실패 0·skip 0. 추가 오류 우선순위 11개는 기존 E2E 내부 검사로 포함했다.
- 사용자 설정 해시 동일, 잔류 테스트 프로세스 없음. 성능/부하·외부 교차 전체·다른 OS 검증 및 머지·커밋·푸시는 하지 않았다.
- 현재 흐름: docs/iteration-4/Snoopykid/iteration-4-walkthrough.md. 상세 결과: docs/iteration-4/Snoopykid/iteration-4-routing-verification-results.md.

## Iteration 4 승인 성능 재측정 완료 (2026-11-25)

- f32e17d와 현재 소스를 격리 -O2 빌드하고 동일 C 백엔드1KiB·C 부하 생성기로 직접 기준선/S1~S6/S_MAX를 측정했다. 조건별5초 워밍업/3회, 총48회 모두 완료했다.
- 검증된 성공응답 165,929건, 실패/5xx/워밍업 실패0건. 고정률 예정 건수 미달0, 측정 전후 FD증가0. 양쪽 S1 품질 상한 통과.
- 포화 조회 중앙값 전 3920.8 → 후 3859.9req/s (-1.55%). 3회 변동 범위가 겹쳐 확정적 개선/저하를 단정하지 않는다.
- 현재 WSL/공통 C 도구에서 보완 전체 묶음을 비교한 결과다. 외부 Linux/Go 보고서의4937req/s 유지 여부나 P2만의 인과 효과로 표현하지 않는다. CPU·RSS·직접접속 추가지연·A/B분산도 보고서/원시자료에 기록했다.
- 사용자 설정 SHA256 동일, 현재 제품 src/Makefile이 측정 스냅샷과 동일하며 server/test_backend/load_client 잔류 프로세스가 없다. 서버 정책/최적화·추가 측정·머지·커밋·푸시는 하지 않았다.
- 상세: docs/iteration-4/Snoopykid/iteration-4-performance-results.md. 보존할 원시 텍스트/CSV/지문: 같은 폴더 performance/. 실행 바이너리와 전체 로그는 build/performance/에만 보존한다.

## Iteration 5 시작 (2026-11-25)

현재 브랜치 Iteration5/concurrency_model-YYJ, 기준 커밋 577a554. A안 채택 및 남은 선택은 docs/iteration-5/Snoopykid/iteration-5-requirements.md, 검증 후보는 iteration-5-tests.md에 기록했다. Wiki에도 비교 원칙과 A안 결정을 저장했다. 현재 제품 코드 변경·검증·성능 실험은 실행하지 않았다.

## Iteration 5 연결 수락·조회 분배 추가 결정 (2026-11-25)

부모 accept 후 IPC FD 전달을 채택했다. 조회 분배 상태는 전체 작업자 기준 공유 방향을 채택하되 RR을 최종 확정하지 않는다. RR, Least Outstanding+동률 RR, 응답 시간+동률 RR, 진행 수·시간 결합 점수+동률 RR을 비교 후보로 등록했다. 측정 구간·평균·초기값·공식·동률·실패 반영 및 공유/동기화/작업자 사망 회수는 구현 전 별도 선택이다. 비교 계획은 docs/iteration-5/Snoopykid/iteration-5-distribution-comparison.md 참조. 성능 실행 허락은 없으며 제품 코드 변경·실험은 하지 않았다. 앞선 미결정 기록 중 이 결정과 겹치는 부분은 이 절이 우선한다.

## Iteration 5 공유 메모리 직접 선택 확정 (2026-11-25)

부모 accept/FD IPC/작업자 관리, 작업자 공유 상태 직접 선택/중계/결과 갱신을 채택했다. 선택·진행 수 예약은 한 짧은 동기화 구간이고 I/O는 잠금 밖이다. 공유 메모리 API·동기화/사망 복구·기여분 회수·증감 시점·시간/점수·정책 입력은 미결정. 네 후보 선택 실행 구조는 허용하지만 최종 알고리즘·자동 재시도/제외/전환·성능 실행은 승인하지 않았다. 상세와 제안 흐름은 docs/iteration-5/Snoopykid/iteration-5-shared-state-design.md 참조.

## Iteration 5 공유 메모리/복구 목표 1-A·2-A 채택 (2026-11-25)

fork 전 익명 공유 mmap 상속과 process-shared robust mutex, 복구 후 계속 운영 목표 확정. shm_open/독립 프로세스 참여 제외. WSL의 해당 API 초기화·기본 잠금 확인용 probe 컴파일/실행 성공. 다섯 장애 상황·한계·비용·검증 기준을 docs/iteration-5/Snoopykid/iteration-5-recovery-review.md에 기록. 자료구조·구체 복구 프로토콜·복구 불가 처리·재시작 제한·성능 실행은 미확정/미승인이며 제품 코드 변경 없음.

## Iteration 5 복구 범위·구조 A 승인과 첫 코드 단위 (2026-11-25)
①~④ 복구·재복구, ⑤ 탐지 모순의 제한 진단과 신뢰 불가 시 전체 안전 종료, 상한 있는 구조 A를 채택했다. 재시작 제한·실행 상한·통계/알고리즘 세부는 별도다.
src/shared_backend_state.h/c에 capacity/budget 검증과 mmap/robust shared mutex 초기화/해제를 추가했다. main은 아직 연결하지 않았다. test_shared_backend_state가 ASan/UBSan 컴파일/실행 성공: 상한/초기화와 실제 자식 SIGKILL의 EOWNERDEAD 확인. 카운터 거래/복구·재복구·민감정보 없는 진단·전체 종료는 후속 구현이며 통과로 주장하지 않는다.
현재 메모리 예시 W4/R10000=320376bytes(통계/연결 버퍼 등 제외), 고정 undo record1개120bytes는 header216에 포함. 상세: docs/iteration-5/Snoopykid/iteration-5-shared-state-initialization.md. 성능·부하 미실행, 커밋/푸시 없음.

## Iteration 5 공유 원장 갱신·재복구 단위 (2026-11-25)
고정 before-image transaction, generation token 완료/회수, PREPARED rollback·COMMITTED 보존, owner-death 재복구, 불변 조건·fatal 진단 구현. HTTP 정책/알고리즘/시간 통계는 미변경. main/IPC/epoll/전체 종료 통지는 미통합.
make test 10개 및 최종 신규 경계 테스트 통과, 실제 fork/SIGKILL의 변경·재복구·세대회수·손상·경합 검사. 정상/장애 서버 E2E와 성능 통과로 확대하지 않는다. header232/undo128, W4/R10000 예시320392bytes+로컬scratch96bytes/프로세스. 상세 docs/iteration-5/Snoopykid/iteration-5-reservations-review.md.

## Iteration 5 IPC 채널 선택 (2026-11-25)

사용자 2번: AF_UNIX socketpair SOCK_STREAM 채택. 부분 송수신·메시지 프레이밍·SCM_RIGHTS 귀속을 명시해 구현할 방향. worker 배분·큐·인계·종료 정책은 미정. 문서만 갱신했으며 코드/검증/성능 변경 없음.

## Iteration 5 worker 선택 로직 재사용 요청

조회 A/B와 worker 선택의 공통 계산 재사용 방향을 요청받았다. 기존 RR 자동 채택은 하지 않으며 상태/소유권/시간 의미는 별도다. worker에 네 정책 전체를 제공할지 범위 확인 후 구현. docs/iteration-5/Snoopykid/iteration-5-ipc-design.md 최신 기록 참조.

## 사용자 확정 — worker에도 네 정책 선택 지원 (2026-11-25)
사용자 1번: worker 분배에도 RR / Least Outstanding+동률RR / 응답시간+동률RR / 진행수·시간 결합+동률RR 네 후보를 선택 실행할 수 있게 구현한다. backend와 후보 비교·점수·동률 계산을 공유하며 불필요한 범용 프레임워크는 만들지 않는다.
worker/backend 상태·순번·정책 설정·시간 측정·표본·실패 반영은 별도 관리한다. 양쪽에 같은 알고리즘을 강제하거나 최종 알고리즘을 확정한 것이 아니다. 기존 parent accept/FD 전달·worker backend 직접 선택·SOCK_STREAM 결정 유지.
worker의 outstanding 기준(미종료 연결/HTTP 처리중 요청), 시작·종료 시점, 부모 통보·세대별 회수, 시간 구간·평균·초기값·노후화·점수·동률·실패 반영은 미결정. backend 통계의 의미를 worker에 자동 적용하지 않는다.
worker/backend 정책을 동시에 바꾼 결과를 한 알고리즘의 효과로 해석하지 않도록 비교 계획에서 다른 층의 정책을 고정하는 조건을 먼저 제시한다. 조합 비교·부하·성능 실행은 별도 허락 필요. 이번에는 코드·검증·성능을 실행하지 않았다.

## worker 진행 수 선택 (2026-11-25)

사용자1번: 배정 후 아직 종료되지 않은 연결 수 채택. 실제 HTTP 처리 중 요청 수만 세지 않는다. 정확한 증가·인계·취소·종료 통보/사망 회수·ACK/상한은 후속 선택. 코드·검증·성능 변경 없음.

## FD 인계 확정 — 사용자 1번 (2026-11-25)
부모는 FD+연결 정보 전체 프레임 전송 완료 시 원본 FD를 닫는다. 수신/등록 ACK는 요구하지 않는다. worker는 프레임 전체 검증 전 client 처리를 시작하지 않는다. 첫 양수 sendmsg 뒤에는 FD를 다른 worker로 자동 재배분하지 않는다. 부분 송신 후 재개에는 동일 ancillary를 재송신하지 않는다. ACK는 미구현/미사용.
첫 구현 흐름은 큐·정책·worker 수와 독립인 fixed-size STREAM FD 채널이다. frame48bytes: magic/version/type/worker index/generation/connection ID/accept시각/예약0. 모두 고정폭 명시 직렬화, C struct padding이나 요청 데이터를 전송하지 않는다. 첫 바이트 recvmsg를 따로 읽어 SCM_RIGHTS 귀속을 고정하고, 이후 현재 frame의 남은 길이 이상 읽지 않는다. 한 channel의 송신 frame은 interleave하지 않는다.
receiver는 단일 수신 FD만 소유하며 잘못된 frame/ancillary/부분 EOF/이전 세대이면 받은 FD를 정리하고 channel 실패를 반환한다. 실제 channel close·parent 전체 종료·worker생성/배분/큐/포화/시간제한은 후속 흐름. 이 구현이 전체 동시성 서버 완료를 의미하지 않는다.

## Iteration 5 STREAM FD 인계 단위 완료 (2026-11-25)
SOCK_STREAM fixed frame48bytes, 첫 양수 sendmsg에 FD1회·전체 송신 후 원본 close/noACK·전체 수신 검증 뒤 worker 소유권 인계 구현. 원본/수신/queued FD 참조의 partial/오류/실제 fork SIGKILL 회수 검증. 일반 컴파일 성공, ASan/UBSan make test 11개 통과.
상세 docs/iteration-5/Snoopykid/iteration-5-ipc-transfer-review.md. main/운영 worker/epoll/queue/정책·fatal 전체 종료는 다음 단위이며 미통합. 서버 E2E/성능 측정/commit/push 없음.

## parent 전역 FIFO 채택 (2026-11-25)

사용자1번: 미배정 전역 FIFO, 전달 시작 시 worker 선택/미종료 수 증가. worker별 대기 FIFO 미채택. Q/W/C/G 및 shared R/예산·포화는 별도 선택. 초기 수치 제안과 정적 자원 산식은 docs/iteration-5/Snoopykid/iteration-5-parent-queue-design.md 참조. WSL FD 한도 읽기만 수행, 설정 변경/성능 측정 없음.

## 초기 상한 묶음 채택 — 사용자 1번 (2026-11-25)
W=4 고정 프로세스 작업자, Q=128 parent 미배정 전역 FIFO, C=256 worker별 배정 후 미종료 연결, G=1152 전체 accepted 미종료 연결, R=1024 공유 backend 예약 슬롯, 공유 mapping budget=1048576bytes(1MiB)를 초기 임시값으로 채택했다. 최적값/성능 측정 결과가 아니다.
G=Q+W*C, R=W*C. 미배정 FIFO는 worker outstanding에 포함하지 않지만 전체 accepted 수에는 포함한다. SENDING/SENT/worker 처리로 이동해도 같은 연결을 중복 계산하지 않는다. partial sender는 worker assigned 상태의 일부다.
parent 보유 client 원본 FD 최대Q+W=132, 현재 shared layout33160bytes≈32.4KiB, validation scratch96bytes/프로세스라는 앞선 산식은 현재 타입 기준 예산 설명이며 실제 전체 메모리/RSS 상한이 아니다. budget1MiB를 전부 선할당하지 않고 필요한 길이만 mmap. 통계/다른 구조 추가 후 산식 재계산·budget 검사.
기존1만 연결 검토 목표를 대체하지 않으며 더 큰 상한 적용/비교는 별도 합의. worker/backend 네 알고리즘은 최종 미확정. 큐 포화 처리·대기/partial 기한·실행 설정 입력·epoll 모드·fatal 종료/재시작은 이번 수치 승인에 포함하지 않는다.
아직 코드의 실행값을 설정하거나 main에 적용하지 않았다. 포화 동작 선택 후 합의된 parent queue/control 흐름을 구현한다. 부하/성능 실행 승인 아님.

## 포화 동작 확정 — 사용자 1번 (2026-11-25)
Q 또는 G 상한에 도달하면 새 accept를 보류, 여유가 생기면 재개. 기존 client/IPC/종료 처리는 계속. HTTP503/accept후즉시닫기는 추가하지 않는다. kernel backlog의 대기/연결 실패 가능성과 app queue의 구분 유지.
첫 구현 단위: 고정 배열 ring FIFO의 생성/이동/해제와 parent_accept_has_capacity. FIFO는 parent만 소유하고 thread/shared mutex를 사용하지 않는다. accepted FD/ID/accept시각만 저장, parent HTTP 파싱/worker선택은 수행하지 않는다. 상한이 차면 enqueue 자체는 ENOSPC/FD 소유권 유지, 실제 parent loop는 accept 전 gate를 확인해 그 상황을 예방한다.
초기 W4/Q128/C256/G1152/R1024/1MiB는 concurrency_limits.h 한 곳에 기록한다. 현재 main 실행값/CLI에 적용하지는 않는다. parent 전체 연결 장부/worker 종료통보/epoll 등록·해제는 후속 통합이며 gate가 존재한다고 서버가 이미 포화 보류 동작을 수행한다고 주장하지 않는다.

## Iteration 5 포화 보류 확정·FIFO 단위 완료 (2026-11-25)
사용자1번: Q/G full이면 accept보류, 기존연결/IPC/종료 계속, 여유시재개. 초기상한을 concurrency_limits.h에 기록하고 parent-only 고정FIFO와수락gate 구현. main실행값/epoll/전체장부는 미통합.
일반컴파일과make test ASan/UBSan12개통과. 실제loopback 테스트driver의gate보류/재개·STREAM인계·FD회수 확인. FIFO배열4096+owner32bytes, 전체parent장부·socket·workerbuffer별도. 상세 docs/iteration-5/Snoopykid/iteration-5-parent-queue-review.md. 서버E2E/성능/commit/push 없음.

## Iteration 5 epoll LT 확정 (2026-11-25)

사용자1번: LT로시작, ET/ONESHOT미채택. 긴급부모통지 경로/진단보존/시그널대기·종료정책은별도선택. docs/iteration-5/Snoopykid/iteration-5-control-notification-design.md에대안기록. 새구현/검증/성능미실행.

## 긴급 경로 채택 — 사용자 1번 (2026-11-25)
일반 CLOSED는 기존 STREAM worker→parent, 긴급 알림은 전역 nonblocking pipe1쌍+worker별 고정 진단칸을 채택. client FD STREAM/noACK는 유지. 고정 진단은 reporter와대상worker/세대·검사/phase/stage·종료이유 숫자만, 가변문자열/body/headers/URI 없음.
첫 단위는 공유slot 초기화/arming·원자게시·읽기·boundedformat과pipe 생성/알림/소비. slot은 worker당1개(상한W), firstreport를 같은세대에서 덮어쓰지 않음. parent는 초기fork전 또는 waitpid로종료확인후만 다음세대를arm, 자동재시작은하지않음. slot publication EMPTY→WRITING→READY, ready전payload를읽지않음. 게시도중사망은 incomplete/unknown으로분류하고원인을추정하지않음.
parent의실제SIGCHLD/epoll 대기경합·로그sink적체/강제종료기한·전체종료는후속통합. 특히 게시후pipe쓰기전사망은pipe알림이없을수있어SIGCHLD 경로가필요함. 이단위가그전체wake/종료보장을완성했다고주장하지않는다.

## Iteration 5 별도긴급pipe 채택·진단단위 완료 (2026-11-25)
사용자1번: 별도nonblocking pipe+worker별고정진단. publication/firstreport/generation·bounded숫자/unknownformat 구현, 실제LT/pipe포화·STREAM적체·게시5경계SIGKILL/복구불가mutex 검사통과. make testASan/UBSan13개성공.
slot64bytes W4추가256, 초기shared33416bytes(1MiB예산안). 상세 docs/iteration-5/Snoopykid/iteration-5-failure-channel-review.md. main/parent전체종료·SIGCHLD/로그sink·종료기한은다음통합, 서버E2E/성능/commit/push없음.

## 사용자 확정 — 1번 (2026-11-25)
epoll_pwait + 최소 sig_atomic_t handler를 채택했다. signalfd/시그널전용FD는 추가하지 않는다. 모듈은 caller가 보유한 제어 상태와 OS handler를 받아 새 가변 전역 상태를 만들지 않는다. handler는 종료0/실행1/자식변경을포함한실행3만 남기고, 종료 뒤SIGCHLD가 와도실행으로 되돌리지 않는다. 관리handler의sa_mask에관리시그널전체를넣어중첩갱신을막는다.
첫단위는mask/disposition 설치·복원, epoll_pwait, child-event표시 소비와exec자식mask/default복원이다. 실제waitpid는하지않아부모worker/작업자CGI단일회수책임을유지한다. main/기존blockingCGI는아직연결하지않는다. caller는대기전상태를확인하고EINTR을무조건재시도하지말아야한다.

## Iteration 5 시그널 단위 미완료·반례 실패 (2026-11-25)
epoll_pwait/최소handler/설치복원 모듈과신규단위추가, 컴파일성공·대기경계6통과. 하지만ready I/O가계속될때pending종료를처리하지못하는검증실패. 새단위포함make test전체통과아님, main미통합.
수정안은 docs/iteration-5/Snoopykid/iteration-5-signal-review.md. 반환후handler전달구간(mask변경2회) 또는pending조회/0초소비의동작·비용을설명하고사용자선택대기. 테스트를제거/완화하지않았고기능·성능정책자동변경없음. 기존13개앞선통과와현재신규실패를구분한다.

## wait 반환 구간 보강 확정 (2026-11-25)
사용자 ‘1반’을 1번 선택으로 확인했다. epoll_pwait 반환 직후 wait_mask를 잠깐 적용해 pending 관리 시그널을 handler에 전달하고 managed_mask를 다시 차단한다. caller는 I/O보다 종료·fatal·자식 상태를 먼저 검사한다. 두 mask 호출이 성공하면 원래 wait 반환값과 errno를 보존한다. mask 변경 실패는 -1로 보고하고, 첫 호출 실패 시에도 재차단을 시도한다. 호출마다 mask 변경 두 번이 추가되며 성능은 측정하지 않는다. 기존 ready I/O 실패 반례는 유지한다.
검증 범위: 기존 시그널 시험 전체, ready 반환값·차단 마스크, EINTR/EBADF 보존, timeout 반환, 기존 단위 회귀. main 통합·실제 CGI·부하 측정은 포함하지 않는다.

## 시그널 보강 검증 완료 (2026-11-25)
1번 구현 후 기존 ready I/O 실패 반례와 시그널·fork/exec·FD 회수 시험 통과. 일반 O2 컴파일 성공, ASan/UBSan 전체 make test 14개 성공(exit 0). 반환값·errno·차단 mask 보존도 확인했다. sigprocmask 실패 강제 주입, main/실제 worker/CGI 통합 E2E는 미실행. 성능 미측정. 상세 흐름·비용·한계는 iteration-5-signal-review.md에 기록했다.

## 다음 구현 제안: 연결 장부·CLOSED 통지 (2026-11-25)
시그널 단위 완료 후 부모의 accepted 장부·worker 종료 통지·capacity 회수 흐름을 설계했다. 기존 STREAM 역방향 사용은 확정 사항이다. 새 선택은 A: worker별 bounded CLOSED 큐+부모 보수적 장부(추천), B: 연결 원장까지 공유 메모리 확대다. 아직 선택/구현하지 않았다. 소유권·빠른 완료 경합·대략 메모리·검증 후보는 iteration-5-connection-completion-design.md에 기록했다. 알고리즘/자동재시작/성능을 확정하거나 실행하지 않는다.

## 사용자 A(1번) 확정 — 부모 장부와 bounded CLOSED 큐 (2026-11-25)
부모 전용 G칸 연결 장부와 worker별 C칸 완료 큐를 채택했다. parent는 완전한 CLOSED 전까지 보수적으로 점유 수를 유지한다. sender 정리가 끝나기 전 CLOSED 도착은 표시만 남겨 슬롯 재사용을 막는다. 장부는 FD를 소유하지 않고 FIFO/FD sender/worker가 기존 소유권대로 정리한다. 사망 회수는 waitpid 확인 및 부모 sender abort 후 호출한다. 토큰은 기존 64bit connection_id에 32bit 재사용 세대와 32bit 슬롯+1을 담는다. 세대 wrap은 오류다. 완료 프레임은 고정 32byte로 worker/세대/connection_id를 검증한다. 중복·이전 세대 무변경, 미래·다른 소유권·잘못된 형식은 EPROTO 보고. 종료 범위/자동재시작은 이 단위에서 정하지 않는다.
코드 순서: create_parent_connection_ledger → register_parent_connection → assign_parent_connection → finish_parent_connection_handoff / apply_worker_connection_closed → reclaim_reaped_worker_connections. worker는 enqueue_worker_connection_closed → advance_worker_closed_send, parent는 advance_worker_closed_receive → apply. 큐 full이면 caller가 새 FD 수신을 보류하고 기존 I/O·제어를 계속한다. 이 모듈이 실제 epoll interest/main을 변경하지 않는다.

## 연결 장부·CLOSED 단위 완료 (2026-11-25)
사용자 1번을 parent_connection_ledger.h/c 및 worker_closed_channel.h/c로 구현했다. QUEUED/SENDING/OWNED와 early CLOSED, 토큰 재사용 세대, 정확1회 해제·waitpid 이후 회수, bounded worker 큐와32byte STREAM 부분 I/O를 제공한다. 장부/완료 큐는 FD를 소유하지 않는다. 실제 W4/C256/G1152 경계·부분 I/O/실제EAGAIN·비정상 ancillary·3경계 SIGKILL 회수 포함 ASan/UBSan make test 15개 통과. 일반O2 컴파일 성공. 장부+완료큐+수신 상태62,104bytes(현재ABI), 다른서버자원 제외. main/실제epoll/서버E2E·성능 미통합/미실행. 상세 iteration-5-connection-completion-review.md. 이전 ‘제안/미선택’ 기록은 이 선택과 결과로 대체한다.

## 다음 통합: 부모 감독 루프 (2026-11-25)
기존 확정 결정은 유지하고 main→fork→부모/worker epoll→사망 회수 흐름을 iteration-5-supervisor-design.md에 정리했다. 새 미결정은 제어 채널 오류의 격리 범위와 종료 유예/SIGKILL 정책이다. 추천은 해당 worker만 격리·임시2초 후 SIGKILL·waitpid 뒤 회수, 나머지 계속 서비스·유효worker0이면 전체 오류 종료. 자동재시작 없음. 아직 사용자 선택 전이며 코드/테스트/성능은 실행하지 않았다.

## 감독 정책 1번 확정 (2026-11-25)
채널 오류 worker만 배정 중단·SIGTERM, 임시2000ms 후 살아 있으면 SIGKILL 한 번, waitpid 확인 후 sender/채널·연결 장부·backend 예약 회수. 나머지는 계속 서비스하며 자동대체 없음. 모든 유효worker소실은 전체 오류 종료. 사용자 종료/shared fatal도 같은 종료 유예 적용. 신뢰 불가 shared상태면 전체 종료하되 실제 자식 모두 회수 전 mutex/mapping을 파괴하지 않는다.
첫 구현은 부모 감독 상태와 OS kill/waitpid/회수 연결, 실제 fork/epoll fixture다. main/HTTP worker 상태머신·로그sink·부모사망·분배점수는 미통합. 공유mutex EBUSY는 pending으로 반환하고 다른worker/종료처리를 계속한다. 실제 부모루프의 재회수 깨우기/간격은 아직 자동 선택하지 않는다.

## 부모 감독 단위 완료 (2026-11-25)
parent_worker_supervisor.h/c에 PID별 WNOHANG·격리·임시2000ms 유예/TERM/KILL 각1회·waitpid 확인 후 sender/채널→연결장부→backend 원장 회수를 구현했다. shared EBUSY는pending, 신뢰불가면전체종료, 유효worker0 오류종료, 자동대체없음. 첫숫자진단고정보존·ECHILD시PID신호/파괴차단. 일반모듈O2와make all성공, 실제fork/신호/잠금경합 포함ASan/UBSan make test16개통과. W4추가560bytes(현재ABI), 성능미측정. 상세 iteration-5-supervisor-review.md. main/실제HTTP worker·log sink·부모사망·정책점수·EBUSY 재시도 wake/간격은미통합/미결정이며 테스트의20ms를운영정책으로채택하지않음. make verify/부하/커밋/머지/푸시없음.

## 다음 선택: 부모 제어 재시도 깨우기 (2026-11-25)
EBUSY 회수와 waitpid EINTR 재확인을 추가 외부요청 없이 진행하고 ready I/O에서 반복 trylock하지 않도록 제안했다. 1추천: pending worker별 임시10ms 기한+기존epoll timeout 통합, 정상unlock 추가IPC없음. 2대안: unlock통지+fallback기한·별도경합설계. 아직 선택/코드/실행없음. 상세 iteration-5-supervisor-retry-design.md. 기존테스트20ms는운영값으로채택하지않았다.

## 재시도 1번 채택 (2026-11-25)
worker별 pending 종류(WAIT 재확인/BACKEND 회수)와 다음 기한을 고정 상태에 둔다. EBUSY/백엔드 회수 및 waitpid EINTR 뒤 now+임시10ms를 예약한다. 기한 전 해당 작업만 건너뛰고 다른worker/긴급·종료처리는 계속한다. 다음 epoll 대기는 신호 종료기한과 제어 재시도기한의 최소값이다. pending이 없어지면 주기 wake를 없앤다. 새IPC/FD/정상unlock 통지는 추가하지 않는다. shared fatal의 종료는 재시도기한을 기다리지 않는다.

## 10ms 제어 재시도 완료 (2026-11-25)
사용자1번을 worker별 WAIT/BACKEND retry_kind/retry_at_ms와INITIAL_SUPERVISOR_RETRY_MS=10으로 구현했다. 기한전같은작업보류·due시1회·pending해제·sharedfatal우선, parent_supervisor_wait_timeout_ms로종료/재시도최소기한 제공. 추가요청없는실제epoll타이머회수·ready I/O trylock제한·worker별기한·새CHLD없는EINTR재회수 통과. 일반O2/make all성공, ASan/UBSan전체16개통과. W4감독저장592bytes(이전보다32증가). 상세 iteration-5-supervisor-retry-review.md. main/HTTP worker미통합, E2E/성능미실행. 이전재시도방식미결정문구는이번선택으로대체.

## 분배 로직 다음 선택: backend outstanding 수명 (2026-11-25)
기존 worker 미종료연결 기준은 유지. backend 예약시작/종료는 아직제안임을 확인했다. 1추천은 요청본문완료 후선택+예약→backend framing완료/실패/취소시해제(잔여client출력과분리), 2대안은client연결종료까지유지. half-close를취소로오인하지않음·완료lock EBUSY토큰보존검증후보 포함. 상세 iteration-5-backend-lifetime-design.md. 아직선택/구현/측정없음.

## backend 진행 수 수명 1번 확정 (2026-11-25)
요청본문완료 후 공유잠금 아래 선택+예약→connect/송신/응답 수신→backend 프레이밍 검증 완료 또는 실패/취소에서 해제 요청한다. 남은client출력/마무리와 분리. 역압력으로backend완료미확인시 ACTIVE유지. half-close자체는취소가아님. 완료lock EBUSY는RELEASE_PENDING토큰보존, 공유감소확인전상태재사용금지. worker측재시도간격은아직미결정이며부모10ms자동적용없음.
첫구현단위는worker연결이소유하는backend_request_lifetime: begin_locked(기존잠금/선택결과로예약), mark_finished(완료/실패/취소), try_release(1회trylock과해제), reset(해제확인후만). FD/본문/프레이밍parser를소유하지않으며실제HTTP상태머신연결은다음단위다. 성공판정은parser가프레이밍검증후호출하며임의EOF를성공으로바꾸지않는다.

## backend lifetime 단위 완료 (2026-11-25)
backend_request_lifetime.h/c에begin_locked→ACTIVE→mark_finished→RELEASE_PENDING→try_release→RELEASED/reset구현. 공유선택과예약동일잠금·EBUSY토큰보존·중복첫outcome유지·재사용보호. 실제fork경합/3경계SIGKILL·parent회수·잔여client출력/worker수분리·half-close socketpair시험포함ASan/UBSan전체17개통과, 일반O2/make all성공. 객체32bytes(현재ABI). 실제HTTP프레이밍완료hook/worker상태머신·release재시도미통합/미결정;기존parser회귀와구분. 성능/E2E/커밋/머지/푸시없음. 상세 iteration-5-backend-lifetime-review.md.

## 다음선택: 분배용시간구간 (2026-11-25)
backend시간(connect직전→최종framing검증완료)과worker시간(FD전체검증인계→연결자원정리/CLOSED enqueue직전)을분리하는1번추천, 최종헤더준비까지의짧은구간2번대안을정리했다. backend해제잠금/잔여client출력과worker CLOSED전송적체는측정구간밖. 아직사용자선택전이며계측/IPC변경/성능실행없음. 평균·초기/노후/실패·식·동률·입력은구간합의후정책묶음으로제시. iteration-5-policy-time-design.md참조.

## 시간구간확정·분배규칙묶음제안 (2026-11-25)
사용자1번으로backend connect직전→최종framing완료/worker FD인계→자원정리완료시간채택. EWMA1/8·미측정RR·32회마다RR탐색·실패최소1초·결합(outstanding+1)*시간·정확동률RR·기존config의worker/query정책필수명시를미채택제안으로정리. 상세 iteration-5-policy-rules-design.md. 아직구현/계측/IPC변경/성능실행없음. 기존최종알고리즘미확정유지.

## 분배 규칙 묶음 구현 승인 (2026-11-25)
사용자 ‘구현’: EWMA1/8, 미측정RR, 시간/결합32회마다RR탐색, 실패min1초·client취소/사망표본제외, 결합(outstanding+1)*시간/포화, 정확동률RR, worker/query정책설정필수명시를채택했다. 최종알고리즘/성능실행은미확정이다. 공통계산→shared transaction→CLOSED parent통계→config순으로연결한다. 제품main의HTTP nonblocking구현완료를의미하지않는다.

## 네 분배 정책 구현·단위 검증 완료 (2026-11-25)
공통 distribution_policy(네정책/EWMA1/8/32회탐색/실패floor1초/포화곱/정확동률), sharedv4 통계+선택예약/완료해제 transaction과복구checksum, backend고정시간구간, parent관측CLOSEDv2(56byte)/선택배정, 필수worker-policy/query-policy설정 구현. 순차query경로도정책/표본연결;worker-policy의실제multiworker main연결은아직미통합. 예제config/proxy.conf.example 추가, 개인build설정불변. 일반O2/make all·ASan/UBSan전체18개통과. 통계변경10·선택변경8경계SIGKILL/재복구/2프로세스경합 검증. sharedmapping33528bytes(W4/R1024),상세비용/함수흐름 iteration-5-policy-review.md. 제품make verify/E2E·부하/성능·커밋/머지/푸시없음. 최종알고리즘미정유지.

## 다음 구현 선택: worker 실제HTTP I/O (2026-11-25)
기존프로토콜/상한/시간을유지하며고정16KiBrelay·16/8KiB역압력·worker pending10ms·선택lock1초후503·해제token보존·진행기준idle/역압력중backendidle예산정지/전체기한유지·FD이벤트64KiB/16syscall·batch64·Cslot기한스캔·세대eventtoken의추천묶음을제안. 아직미선택/코드변경없음. 상세 iteration-5-worker-io-design.md. localfile/CGI블로킹전환은별도선택이며기능제거/블로킹loop우회없음. 성능미실행.

## worker I/O 추천 묶음 구현 승인 (2026-11-25)
사용자‘구현’으로16KiB/8KiB역압력·worker pending10ms·선택lock1초후503·해제token보존·진행기준idle/읽기보류중backendidle잔여정지/전체기한유지·64KiB/16syscall·epollbatch64·slot기한스캔/세대eventtoken을채택했다. 첫실행단위는기존FD STREAM을받는실제worker epoll reactor와nonblockingclient/backend상태다. localfile/CGI는미정이므로LOCAL_READY에서외부비블로킹응답준비로넘길경계를노출하고기존main은유지한다. 정적/CGI를꺼버리거나blockinghandler로우회하지않는다.

## 실제 worker nonblocking 네트워크 단위 완료 (2026-11-25)
worker_http_io.h/c·worker_http_internal.h·worker_proxy_io.c에기존FD/CLOSED채널+실제LT epoll다중연결·요청/100·backend선택/connect/송신/헤더/본문·16/8KiB역압력·worker10ms/선택1초503·해제pending·전체/idle기한·예산·세대eventtoken구현. 기존pureheader/parser재사용. FD전달중증거있는TCPreset은CLIENT_GONE으로연결취소처리,다음frame/FD회수검증. strictmake all/worker-io및ASan/UBSan전체20개통과,실제fork/TCP/Cbackend/4MiB binary/느린A중B·제출진전/네정책/SIGTERM대기중회수확인. 상세 iteration-5-worker-io-review.md. 제품main은여전히순차,localfile/CGI/부모실행통합/긴급통지최종조율미완료. make verify제품E2E·부하/성능·커밋/머지/푸시없음.

## 구현 우선 운영 변경 (2026-11-25)
사용자 지시: 확정한 흐름을 이어 구현한다. 중간 확인은 compile/link와 변경에 직접 필요한 최소 기능(정상,느린/빠른동시처리,종료·자원정리)로 제한한다. 전체회귀/광범위사망주입은 실행가능 통합 체크포인트에 모은다. 실패는보고, 성능은별도승인이다.
이번 단위는 parent의실제listener LT·FIFO/분배·부분FD/CLOSED·supervisor·fatalpipe를한루프로연결한다. localfile/CGI의새 I/O선택은미정이므로제품 main전환은아직하지않고 실제fork4worker와Cbackend를사용하는짧은통합실행에서검증한다. 기존main경로의기능을삭제/블로킹worker로우회하지않는다.

## 부모·4작업자 HTTP 통합 최소 확인 (2026-11-25)
parent_http_loop의 단일 리스너→FIFO→worker 정책→SCM_RIGHTS→CLOSED→감독/회수 흐름과 기존 hello/echo/greet 메모리 응답을 실제 reactor에 연결했다. strict O2 컴파일·링크와 짧은 실제 fork/TCP 통합 실행 통과: 느린 A 중 B/제출/로컬 응답 완료, 같은 작업자의 greet 진전, SIGTERM 후 4worker/FD/장부/예약 회수. 최초 테스트 fcntl.h 누락 컴파일 실패는 수정했다. 이번 변경 후 전체 회귀·광범위 장애·성능은 실행하지 않았다. 제품 main은 순차 유지, 정적 파일/CGI I/O는 별도 선택 후 연결한다. 상세 iteration-5-parent-integration-review.md. 실행 make smoke-process-http는 기능 최소 검사이며 성능 측정이 아니다.

## 파일 보조 스레드 1번 채택 및 구현 전 최소 확인 (2026-11-25)
작업자별 제한된 파일 보조 스레드를 채택했다. 첫 구현은 작업자당 보조 스레드 하나(성능 결론이나 최종 튜닝값 아님), 기존 C 연결 상한에서 유도한 작업 큐, 연결별 최대 한 작업이다. 파일 준비/read/provider cleanup만 맡기며 네트워크·공유 backend 잠금은 건드리지 않는다. 기존 16KiB provider 버퍼를 빌리고 현재 조각 송신 후 다음 읽기를 요청한다. 취소 중 작업은 슬롯과 provider를 유지하고 보조 스레드 완료/정리 후 CLOSED를 보낸다. 작업중 디스크 syscall을 강제 취소하지 않는다. 종료는 join을 기다릴 수 있고 기존 부모 2초 유예/SIGKILL/waitpid가 최종 경계다. 파일 전용 완료 pipe를 epoll에 추가한다. CGI fork는 호출하지 않고 별도 비동기/다중스레드 안전 통합으로 남긴다.
최소 확인: 보조 스레드가 테스트용 느린 파일 작업을 수행하는 동안 호출자 진행, 제한 큐/한 작업/정리; 실제 4worker 통합에서 임시 fixture 정적 파일의 다중 조각 바이너리 일치·기존 요청 동시 처리·종료 FD 회수. 전체 회귀·광범위 장애·성능은 이번 실행에서 제외한다.

## 파일 보조 스레드 통합 완료 (2026-11-25)
worker_file_io/worker_file_bridge로 worker당1helper·C상한큐·job당한작업·기존16KiB provider 스트리밍·완료pipe/epoll·취소후정리전slot/CLOSED보류를 구현했다. 일반O2 all/worker-io/두 최소실행 링크성공. 느린파일testhelper중호출자진행·실제4worker 임시fixture128KiB binary/빈파일/404/cafe·느린query중다른요청진전·SIGTERM후FD/예약회수통과. signal mask는pthread_sigmask(일반)/sigprocmask(exec자식)로분리하고 직접관련ASan/UBSan signal대상통과. 최초featuremacro컴파일실패는수정. 전체회귀/광범위장애/TSan/성능미실행. helperjoin은diskcall을기다릴수있고2초KILL도커널D상태회수를보장하지않음. 제품main은기존순차,CGI안전한비동기실행/회수후전환예정. 상세 iteration-5-file-io-review.md.

## CGI 비동기·제품 진입점 연결 전 범위 (2026-11-25)
기존 CGI 경로·Status/Content-Type·chunked·헤더/읽기5초·종료오류 정책을 유지한다. 경로/실행env준비는파일helper,네트워크스레드fork자식은async-signal-safe FD/시그널/exec만사용. 비차단stdout파이프·한조각역압력·PID별WNOHANG단일회수자·취소TERM/기존200ms후KILL을eventloop에연결한다. exec/CGI실패는기존502/500,최종송신후새오류미첨부. helper완료·CGI자식회수전슬롯/CLOSED보류. 기존CLI설정필수·4worker/1listener·공유분배를제품main에연결한다.
최소확인후보: 기존CGI정상/query/바이너리chunk·느린CGI중동일worker빠른로컬응답·잘못된CGI/실행실패·클라이언트취소·종료자식/FD정리,제품실행진입점정상기동/종료. 임시fixture만변경한다. 전체회귀·광범위장애는통합체크포인트후검토,성능미실행.

## CGI·제품 main 통합 체크포인트 (2026-11-25)
기존 CGI 정책을공통함수로추출,helper launch준비→안전한fork/exec자식→nonblocking stdout/청크/역압력→PID별WNOHANG→취소TERM/기존200msKILL·회수로연결. 제품main이실제부모/4worker/epoll/공유분배를실행한다. strictO2제품링크와smoke-file-io/smoke-process-http/smoke-process-server통과. 최초Makefile변수순서링크실패수정. 실제제품CGI정상/잘못된출력/exec실패/비정상종료·느린CGI중모든worker진전·RST취소FD기준선·살아있는CGI중SIGTERM회수통과. 전체회귀/광범위장애/성능미실행. stdoutEOF후계속생존CGI의별도종료기한/worker강제사망시후손감독은미결정후속후보. 이전main순차·CGI미통합기록은이결과로대체. 상세 iteration-5-cgi-main-review.md, 로그cgi-product-build.log/cgi-product-minimal.log.
