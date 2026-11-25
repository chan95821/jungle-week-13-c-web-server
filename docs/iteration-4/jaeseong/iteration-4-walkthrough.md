# Iteration 4 코드 읽기와 실행

상태: 구현 초안 작성. 공식 검증·성능 측정은 미실행. 팀 기준 채택·부모 #4 전체 완료 판정 전.

## 1. 이번에 만든 흐름

```text
학생 → C 서버 :8080
             ├─ POST /api/submissions       → 제출 :8001
             ├─ GET /api/submission-status  → 조회 A :8002 / B :8003
             └─ 그 외                      → 기존 로컬 핸들러
```

조회는 유효한 요청 본문까지 수신한 뒤 A→B→A→B 순으로 목적지를 하나 선택한다.
실패해도 재시도하지 않고 다음 요청은 다음 순번을 사용한다. 이는 이번 개인 구현의 정책이며 공통 계약은 교대 순서·정확한 비율을 강제하지 않는다.
조회 A/B의 개발용 응답은 같은 고정 상태이며 `X-Backend-Id`만 다르다. 제출은 본문 그대로 201로 되돌린다. 실제 저장과 데이터 동기화는 없다.

## 2. 실행 안내 — 아래 명령을 이번 작업에서 실행한 것은 아님

Ubuntu WSL2를 복구하고 gcc·make·python3가 준비된 뒤 저장소 루트에서 실행한다.

```sh
cd /mnt/c/Users/wotjd/Desktop/정글/week13
make
```

백엔드는 각각 별도 터미널에서 실행한다.

```sh
python3 scripts/iteration4_backend.py --port 8001 --role submission --id submission
python3 scripts/iteration4_backend.py --port 8002 --role status --id A
python3 scripts/iteration4_backend.py --port 8003 --role status --id B
```

프록시는 네 번째 터미널에서 실행한다. 인자 순서는 프록시 포트·제출·조회 A·조회 B다.

```sh
./build/server 8080 127.0.0.1:8001 127.0.0.1:8002 127.0.0.1:8003
```

직접 기본 동작을 살펴볼 때 사용할 예시다.

```sh
curl --noproxy '*' -i --data-binary 'student=alice&assignment=13' http://127.0.0.1:8080/api/submissions
curl --noproxy '*' -i http://127.0.0.1:8080/api/submission-status
curl --noproxy '*' -i http://127.0.0.1:8080/api/submission-status
curl --noproxy '*' -i http://127.0.0.1:8080/cafe
```

기존 `./build/server 8080` 실행은 프록시 없이 로컬 서빙만 한다. 정적 루트는 기존처럼 작업 디렉터리의 `public`, CGI 실행 디렉터리는 `cgi-bin`이다. API 경로만 정확 일치로 중계하고 query를 포함한 원본 target은 바꾸지 않는다. 예를 들어 `/api/submissions/extra`는 이 API에 매칭되지 않는다.

## 3. main → handle_client_with_proxy

- `src/main.c`: CLI를 검사해 `proxy_config`를 스택에 만든다. 백엔드 주소는 `parse_backend_address`로 검사한다. `socket/bind/listen/accept`, `htons/htonl`은 POSIX/소켓 API이며 `open_listenfd`, `parse_port`, `parse_backend_address`는 직접 만든 함수다.
- 주소는 숫자 IPv4만 받는다. DNS·IPv6·TLS·CONNECT는 이번 초안 범위 밖이다. `inet_pton`으로 주소를 바이너리로, `htons`로 포트를 네트워크 바이트 순서로 바꾼다. TCP가 이 애플리케이션 설정 변환을 대신하지 않는다.
- `src/server.c`: `handle_client_with_proxy`가 헤더 수신·기존 HTTP 파싱을 먼저 한다. 프록시 설정이 있고 지정 API면 `choose_reverse_proxy_route`로 분기하고 그 외는 기존 라우터를 쓴다.
- 요청 본문은 기존처럼 Content-Length만큼 한 번 할당하며 최대 64KiB다. 이미 헤더와 같이 받은 본문은 복사하고 부족분만 받는다. `Expect: 100-continue`는 기존 수신 흐름이 처리한다.
- 전체 본문을 받은 다음 `handle_reverse_proxy_request`로 넘긴다. 따라서 **요청은 버퍼링, 응답은 스트리밍**이다. 저속 클라이언트 요청을 백엔드에 조금씩 바로 넘기는 구조는 아니다.
- 기존 `handle_client`는 프록시 없는 wrapper로 유지해 기존 호출자·단위 계약을 보존한다.

## 4. handle_reverse_proxy_request — 위에서 아래로 읽기

가독성 정리 후에도 동작·상한·FD 소유권은 같다. 외부 호출자는 기존 `src/proxy.h`만 사용하며, 내부 계약은 `src/proxy/internal.h`에 모았다.

| 파일 | 읽을 내용 |
|---|---|
| `src/proxy.c` | 전체 중계 순서, 주소·경로 해석, 백엔드 선택, HTTP 오류 매핑, FD 최종 정리 |
| `src/proxy/io.c` | connect/poll/recv/send와 timeout, 부분 송신, 연결별 수신 버퍼 |
| `src/proxy/headers.c` | 요청·응답 헤더 문법 검사와 재구성, 중간 1xx 소비 |
| `src/proxy/body.c` | 고정 길이·chunked·EOF 본문 중계와 완료 판정 |
| `src/proxy/internal.h` | 프록시 내부 함수·reader·응답 정보·기존 상수 |

`handle_reverse_proxy_request()`는 다음 순서로 읽는다.

1. `build_backend_request_headers`: 원본 request-line·Host·end-to-end 헤더를 보존하고 연결 전용 필드와 이미 처리한 Expect를 제거한다. Content-Length·Connection: close·Via를 만든다. Connection이 Host를 지명하면 기존 개인 정책대로 400이다.
2. `choose_backend_address`: 제출은 고정 서버, 조회는 `next_status`의 A/B 하나를 선택하고 순번을 이동한다. 연결 실패 시에도 이 순서는 같다. 재시도하지 않는다.
3. `connect_to_backend`: nonblocking connect를 최대 1초 기다리고 SO_ERROR를 확인한다. 내부에서 생성에 실패한 FD는 정리하고, 성공 FD만 handle_reverse_proxy_request에 넘긴다.
4. `send_request_to_backend`: `send_all_bytes`로 헤더 다음에 정확한 요청 본문을 전송한다. short count를 누적하고 송신 진전이 있을 때만 무진행 기한을 갱신한다.
5. `prepare_client_response_headers`: reader로 최종 헤더를 읽고 검사한 뒤 client용 헤더를 만든다. 1xx는 기존 총량 한도 안에서 소비한다. 헤더와 동반 도착한 본문은 reader에 남는다. 프로토콜 오류는 EPROTO로 알리고 handle_reverse_proxy_request가 502로 매핑한다.
6. client를 nonblocking으로 설정하고 응답 송신 경계를 표시한다. 첫 send 시도부터 실패는 -1로만 반환한다. 부분 성공 가능성이 있으므로 그 뒤에는 새 오류 응답을 보내지 않는다.
7. `send_backend_response_body_to_client`: 204/304는 본문 없이 완료한다. 그 외는 Content-Length → chunked → EOF 순으로 분기한다. 이 함수와 하위 함수는 FD를 닫지 않는다.
8. `cleanup`: backend FD를 한 번 닫고 client 플래그를 복원한다. client 최종 close는 main, 요청 본문 free는 handle_client가 계속 맡는다.

본문 세부 흐름은 `body.c`에서 이어 읽는다.

- `send_fixed_length_body_to_client`: reader의 남은 본문부터 선언 길이만큼 전송한다. 중간 EOF는 EPROTO이며 새 오류 응답을 만들지 않는다. `BODY_RAW_BYTES`와 `BODY_AS_CHUNKS` 이름으로 출력 방식을 구분한다.
- `send_chunked_body_to_client`: 크기 줄 → 본문 → CRLF 순서로 소비한다. 확장 문법, 따옴표 값, trailer는 각각 작은 함수가 검사한다. 모든 청크와 trailer가 정상 완료된 뒤에만 최종 0청크를 보낸다.
- `send_body_until_backend_closes`: 받은 바이트를 재청크하고 정상 EOF에만 최종 청크를 보낸다. **길이가 없는 경우 정상 FIN과 의도치 않은 깨끗한 조기 FIN을 구별할 수 없는 한계는 유지된다.**

CS:APP의 RIO처럼 short count를 누적하고 바이트 길이로 처리하지만, `send_all_bytes`, `read_backend_byte`, `read_backend_line`과 본문 전송 함수는 이 프로젝트의 직접 구현이다. HTTP 메시지 경계는 TCP 패킷·recv 호출 경계와 다르다.

함수 이름은 호출 순서에서 대상과 행동이 드러나도록 맞췄다. `backend`는 내부 서버, `client`는 학생 연결이다. `receive`는 실제 소켓 수신, `read`는 수신 버퍼를 포함한 읽기, `parse`는 바이트의 의미 해석, `build`는 보낼 헤더 생성, `send`는 송신을 뜻한다.

```text
choose_reverse_proxy_route()                 요청 경로·메서드로 처리 종류 선택
  → handle_reverse_proxy_request()           프록시 요청 전체 처리
      → build_backend_request_headers()     백엔드용 요청 헤더 생성
      → choose_backend_address()            제출 또는 조회 A/B 주소 선택
      → connect_to_backend()                백엔드 연결
      → send_request_to_backend()           요청 전송
      → prepare_client_response_headers()   백엔드 응답을 읽어 학생용 헤더 준비
      → send_all_bytes()                    학생에게 헤더 전송
      → send_backend_response_body_to_client() 학생에게 본문 전송
```

## 5. 오류·제한·동시성

| 상황 | 동작 |
|---|---|
| 백엔드 즉시 연결 거절 | 응답 전 502 |
| 백엔드 연결·송수신 무진행 1초 | 응답 전 504, 응답 후 불완전 중단 |
| 내부 FD·메모리 자원 부족 | 응답 전 500 |
| 잘못된 백엔드 헤더·프레이밍 | 응답 전 502 |
| client 응답 송신 시도 후 실패 | 새 오류 없이 중단 |
| client 수신·송신 제한 | 기존 정책 유지; 프록시 client 송신 무진행 5초 |
| 서버 SIGINT/SIGTERM | 취소 표시 확인, poll 중단, FD 정리 |

1초는 이번 개인 초안 값이다. 팀 공통 timeout 값·백엔드 코드는 체크포인트에서 확정해야 한다. 호출별 무진행 timeout이며 전체 응답 완료 시간 제한이 아니다. 응답 헤더 8KiB, 청크 줄 8KiB, trailer 총 8KiB를 사용한다. 응답 전체 본문 크기 상한은 없으며 고정 버퍼로 순차 중계한다.

현재 서버는 **순차 블로킹 처리**다. 백엔드 한 곳이 느리면 그 요청이 끝날 때까지 다음 학생의 연결 처리를 못 한다. nonblocking 소켓과 poll은 한 연결의 제한 시간을 구현하기 위한 것이며 #5 동시성 구현이 아니다. #5에서는 accept 이후 실행 모델, 조회 순번 동기화, 종료·연결별 상태 소유권을 다시 정해야 한다.

## 6. 작성한 검증 코드와 실행 상태

- `tests/unit/test_proxy.c`: endpoint·라우트·헤더 제거·응답 프레이밍·청크 크기 경계. 구현 .c를 포함하지 않고 실제 모듈을 링크한다. 기존 기대값은 유지한다. `make test_proxy`는 sanitizer 바이너리 빌드만 하고 `make test`가 실행한다.
- `scripts/verify_iter4.py`: fixture 3개와 실제 C 서버를 띄워 본문·헤더 바이트, A/B 도달, 502/504, 중간 실패, FD 회복, 기존 로컬 경로, 종료를 확인할 기능 검증 후보다. `make verify-proxy`에만 연결했다.
- `make verify`의 Iteration 1~3 회귀는 유지했다. 프록시를 켠 전체 정적·동적·CGI 경계 회귀까지 모두 포함됐다고 주장하지 않는다.
- **실행한 확인**: 두 새 Python 파일 AST 구문 확인. **C 컴파일·단위/E2E/sanitizer 실행은 미확인**. Ubuntu의 ext4.vhdx 부재로 기동 실패하며, 기존 Debian에도 gcc·make·python3 명령을 찾지 못했다.
- 가독성 정리 후 서버·테스트 바이너리 컴파일을 재시도했지만 Ubuntu가 같은 MountVhd/ERROR_FILE_NOT_FOUND로 실패해 make에 도달하지 못했다. 테스트는 실행하지 않았다.
- 공식 검증 범위 확정·실행 보류는 유지한다. 위 명령은 안내이며 실행 허락이나 통과 기록이 아니다.
- 부모 #4 원문 확인, 공통 백엔드·timeout·분산 요청 묶음 확정 후 채택 필수 조건과 대조해야 한다.
- 부하 생성·성능 계측·최적화는 구현·실행하지 않았다. #5/#6 비교와 채택 측정도 별도 합의 대상이다.

표준 근거: [RFC 9110 연결별 헤더 처리](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.6.1), [RFC 9112 응답 길이](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3), [청크 전송](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1).
