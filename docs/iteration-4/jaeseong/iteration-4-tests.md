# Iteration 4 검증 후보 — 실행 전

상태: 범위 검토·확정 및 실행 전. 아래는 먼저 누적한 기준이며 실행 허락·통과 보고가 아니다.

## 공통 시나리오

1. 제출 POST: 빈 본문, NUL·0xff 포함 본문, 분할 전송을 바이트 비교. 제출 서버만 수신.
2. 조회 GET: query 포함 target 보존. 한 요청에 한 백엔드만 수신. 요청 묶음에서 A·B 식별자 모두 관찰(팀 요청 건수는 미확정).
3. 상태 코드·Content-Type·일반 사용자 헤더·중복 응답 헤더 보존. 연결 전용 헤더와 Connection 지명 헤더는 다음 연결로 전달하지 않음.
4. 응답 본문 프레이밍별 정상·빈·분할·바이너리 응답. 헤더와 동반 도착한 본문 누락 없음.
5. 백엔드 즉시 거절 502. 요청 수신 후 3초 무응답은 개인 초안 제한 1초 기준 504. 공통 값은 별도 확정.
6. 응답 전 잘못된 상태 줄·헤더·모호한 길이·상한 초과는 502. 응답 후 조기 EOF·타임아웃은 새 오류 응답 없이 불완전 종료.
7. 실패 뒤 정상 요청. 연결 정리 이후 /proc/<pid>/fd 개수 누적 없음. 기능 검증이며 부하·성능 측정으로 확대하지 않음.
8. 클라이언트 조기 종료·SIGPIPE·SIGINT·SIGTERM에서 FD 회수와 종료.
9. 잘못된 메서드·경로, URI 인코딩, Expect: 100-continue, 기존 본문·헤더 상한 회귀.

## 기존 회귀 후보

- `make test`: 현재 5개 단위 테스트 + 구현 뒤 추가되는 프록시 단위 검증.
- `make verify`: Iteration 1~3 기존 E2E 유지.
- Iteration 4 기능 스크립트는 별도 대상으로 두고 검토·확정 후 실행.
- sanitizer, FD 회수, timeout 검증 범위를 실행 전에 사용자와 확정한다.
- 부모 Issue #4 원문 수령 뒤 필수 중계 테스트 누락 여부 대조.

## 구현 함수별로 먼저 정한 경계

- endpoint: 포트 0/65536/비숫자, IPv4 문법, DNS·IPv6 미지원 입력 거절.
- 경로 선택: 원본 path 정확 일치, query는 별도 보존, 잘못된 GET/POST는 405, 그 외 메서드는 기존 정책 501.
- 헤더 중계: Connection 지명 헤더가 Connection보다 먼저 있어도 제거. Set-Cookie 중복 순서 보존. Host 보존. 프레이밍을 하나로 재생성.
- 응답 파서: HTTP/1.0·1.1, 100→최종 상태, 204·304 본문 없음, 잘못된 상태/필드/중복 CL/TE+CL/미지원 TE는 502.
- 청크: 16진수 오버플로우, chunk extension, data 뒤 CRLF, trailer 상한·문법, 종료 청크 없이 EOF. 정상 종료 전에는 client 종료 청크를 생성하지 않음.
- EOF 응답: 정상 EOF는 재청크 완료, reset·timeout은 종료 청크 생략. 길이 없는 깨끗한 조기 EOF 자체는 탐지할 수 없는 한계 기록.
- FD: backend 소켓 소유자 하나, client flags 복원, main에서 client close. timeout/불완전 응답/클라이언트 reset 반복 후 정상 요청과 FD 수 회복.

## 결과

- 공식 기능·회귀 검증: 미실행.
- 성능 측정·비교 실험: 미실행, 별도 허락 필요.

## 프록시 가독성 리팩터링의 유지 기준 (구현 전)

- public `proxy_parse_endpoint`·`proxy_select_route`·`proxy_forward`의 인자·반환 계약 유지.
- 요청/응답 헤더 바이트, Connection 지명 필드 제거, 중복 필드 순서, chunk/EOF/고정 길이 종료 판정은 기존 프록시 테스트 기대값 그대로 유지.
- 조회 순번 갱신 시점·실패 시 순번·재시도 없음 유지.
- 백엔드 1초·클라이언트 5초·8KiB 버퍼·wire 추가 256바이트와 기존 한도 유지.
- 첫 client 송신 시도 이후 새 오류 응답 금지, backend 단일 close와 client flags 복원, client FD는 main 소유 유지.
- main/server/router/http/handlers, 기존 단위·E2E 소스는 수정하지 않음. 새 파일 연결을 위한 Makefile과 이번 프록시 단위 테스트만 조정.
- 공식 테스트·sanitizer·E2E·성능 실행은 이번 리팩터링 요청만으로 재개하지 않음.

## 2026-11-24 구현 초안 확인

- 추가: `test_proxy` 단위 계약, `verify_iter4.py` 기능 fixture·중계·장애·FD 회복·종료 후보.
- `make test`는 기존 5개 + 프록시 단위 테스트를 실행한다. `make verify`는 기존 회귀, `make verify-proxy`는 별도 신규 기능 검증이다.
- 새 Python 파일 2개는 AST 구문 확인 성공. 실행 결과가 아니다.
- C 컴파일·링크: 미확인. Ubuntu는 ext4.vhdx 부재로 기동 실패, Debian에는 gcc·make·python3가 없음.
- 단위·sanitizer·E2E: 미실행. 검증 범위 확정 보류 유지.
- FD·timeout·SIGPIPE·시그널 종료의 실제 성공 여부와 부모 #4 전체 요구 충족은 아직 판정하지 않는다.
