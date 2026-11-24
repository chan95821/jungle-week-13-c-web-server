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

- `src/main.c`: CLI를 검사해 `proxy_config`를 스택에 만든다. 백엔드 주소는 `proxy_parse_endpoint`로 검사한다. `socket/bind/listen/accept`, `htons/htonl`은 POSIX/소켓 API이며 `open_listenfd`, `parse_port`, `proxy_parse_endpoint`는 직접 만든 함수다.
- 주소는 숫자 IPv4만 받는다. DNS·IPv6·TLS·CONNECT는 이번 초안 범위 밖이다. `inet_pton`으로 주소를 바이너리로, `htons`로 포트를 네트워크 바이트 순서로 바꾼다. TCP가 이 애플리케이션 설정 변환을 대신하지 않는다.
- `src/server.c`: `handle_client_with_proxy`가 헤더 수신·기존 HTTP 파싱을 먼저 한다. 프록시 설정이 있고 지정 API면 `proxy_select_route`로 분기하고 그 외는 기존 라우터를 쓴다.
- 요청 본문은 기존처럼 Content-Length만큼 한 번 할당하며 최대 64KiB다. 이미 헤더와 같이 받은 본문은 복사하고 부족분만 받는다. `Expect: 100-continue`는 기존 수신 흐름이 처리한다.
- 전체 본문을 받은 다음 `proxy_forward`로 넘긴다. 따라서 **요청은 버퍼링, 응답은 스트리밍**이다. 저속 클라이언트 요청을 백엔드에 조금씩 바로 넘기는 구조는 아니다.
- 기존 `handle_client`는 프록시 없는 wrapper로 유지해 기존 호출자·단위 계약을 보존한다.

## 4. proxy_forward — 위에서 아래로 읽기

1. `build_request`: 원본 request-line·Host·end-to-end 헤더를 복사한다. hop-by-hop 필드, Connection에서 지명한 필드, 이미 처리한 Expect와 기존 길이를 제거하고 Content-Length·Connection: close·Via를 만든다. Host가 Connection에 지명된 요청은 400으로 거부한다. 필드가 Connection보다 먼저 등장해도 전체 헤더를 재탐색해 제거한다.
2. 목적지 선택: 제출은 고정 서버, 조회는 `next_status`의 A/B 하나. 설정과 순번은 main 소유이며 버퍼와 FD는 연결별 지역 변수다.
3. `connect_backend`: POSIX `socket/fcntl/connect/poll/getsockopt`를 조합한 직접 구현이다. nonblocking connect가 EINPROGRESS이면 최대 1초 기다리고 SO_ERROR로 실제 연결 성공을 확인한다. poll의 쓰기 준비만으로 연결 성공이라 단정하지 않는다.
4. `write_all`: 헤더와 정확한 요청 본문을 끝까지 보낸다. send의 부분 송신과 EINTR를 처리한다. 한 번 이상 데이터가 진행하면 무진행 기한을 갱신하며 EINTR 자체는 갱신 이유가 아니다.
5. `reader/read_header/parse_response`: 8KiB reader에 받은 데이터는 위치+길이로 보관한다. 헤더 뒤 본문이 이미 도착해도 잃지 않는다. 최종 상태 전에 온 100/102/103은 내부 소비하고, 101 Upgrade는 지원하지 않는다. 중간 응답 총량도 8KiB로 제한한다.
6. `build_response`: 최종 상태와 일반 응답 헤더를 순서대로 복사한다. Set-Cookie 같은 중복 필드도 병합하지 않는다. 연결 전용 헤더는 제거하고 길이·전송 코딩·Connection·Via를 재생성한다. CL과 TE 동시 존재, 중복 CL, 잘못된 상태·헤더, 미지원 전송 코딩은 응답 전에 502다.
7. 첫 client send 시도부터는 결과를 -1로 유지한다. send가 일부만 성공했는지 확신할 수 없으므로 그 뒤 실패에는 새 HTTP 오류를 보내지 않는다.
8. `relay_exact`: Content-Length만큼만 보낸다. 중간 EOF면 선언 길이에 모자라게 끝나므로 클라이언트가 불완전 응답을 판정할 수 있다.
9. `relay_chunked`: 백엔드 청크 문법과 크기를 검사하고 데이터를 재청크한다. 데이터 끝 CRLF와 최종 trailer까지 정상 확인한 뒤에만 종료 청크를 생성한다. extension·trailer는 소비하고 전달하지 않는다. trailer는 총 8KiB까지 문법 검사한다.
10. `relay_until_eof`: 길이·TE가 없으면 받은 바이트를 청크로 바꾸어 보낸다. 정상 EOF에만 종료 청크를 보낸다. reset/timeout은 불완전 종료한다. **길이가 없는 경우 정상 FIN과 의도치 않은 깨끗한 조기 FIN을 구별할 수 없다.** 중간 종료 검증은 명시적 CL 또는 chunked 응답으로 수행한다.
11. `cleanup`: backend FD를 정확히 한 번 닫고 client의 기존 파일 상태 플래그를 복원한다. client FD를 닫는 주체는 여전히 main이다. 요청 메모리는 handle_client의 cleanup에서 한 번 free한다.

CS:APP의 RIO처럼 short count를 누적하고 바이트 길이로 처리하지만, `write_all`, `reader`, `relay_*`는 이 프로젝트에서 직접 작성한 함수다. HTTP 메시지 경계는 TCP 패킷·recv 호출 경계와 다르다.

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

- `tests/unit/test_proxy.c`: endpoint·라우트·헤더 제거·응답 프레이밍·청크 크기 경계. `make test_proxy`는 sanitizer 바이너리 빌드만 하고 `make test`가 실행한다.
- `scripts/verify_iter4.py`: fixture 3개와 실제 C 서버를 띄워 본문·헤더 바이트, A/B 도달, 502/504, 중간 실패, FD 회복, 기존 로컬 경로, 종료를 확인할 기능 검증 후보다. `make verify-proxy`에만 연결했다.
- `make verify`의 Iteration 1~3 회귀는 유지했다. 프록시를 켠 전체 정적·동적·CGI 경계 회귀까지 모두 포함됐다고 주장하지 않는다.
- **실행한 확인**: 두 새 Python 파일 AST 구문 확인. **C 컴파일·단위/E2E/sanitizer 실행은 미확인**. Ubuntu의 ext4.vhdx 부재로 기동 실패하며, 기존 Debian에도 gcc·make·python3 명령을 찾지 못했다.
- 공식 검증 범위 확정·실행 보류는 유지한다. 위 명령은 안내이며 실행 허락이나 통과 기록이 아니다.
- 부모 #4 원문 확인, 공통 백엔드·timeout·분산 요청 묶음 확정 후 채택 필수 조건과 대조해야 한다.
- 부하 생성·성능 계측·최적화는 구현·실행하지 않았다. #5/#6 비교와 채택 측정도 별도 합의 대상이다.

표준 근거: [RFC 9110 연결별 헤더 처리](https://www.rfc-editor.org/rfc/rfc9110.html#section-7.6.1), [RFC 9112 응답 길이](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3), [청크 전송](https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1).
