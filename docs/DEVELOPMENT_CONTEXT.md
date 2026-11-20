# 개발 진행 맥락

## 범위와 원칙

- 이 저장소는 C로 만드는 HTTP 웹 서버다.
- 구현 순서는 GitHub Issues `#1`~`#6`의 Iteration 계획을 따른다.
- Proxy 범위는 **Reverse Proxy만**이다. Forward Proxy와 HTTPS `CONNECT` 터널링은 구현하지 않는다.
- 매 Iteration은 각 팀원이 설계, 구현, 검증을 독립적으로 수행한 뒤 체크포인트에서 비교하고 하나의 기준 구현으로 통합한다.
- `main`에는 해당 체크포인트의 회귀 테스트를 통과한 코드만 둔다.

## 브랜치 전략

| 목적 | 이름 | 규칙 |
| --- | --- | --- |
| 검증된 기준선 | `main` | Iteration 통합 검증 후에만 병합한다. |
| Iteration 통합 | `iteration/<n>-<topic>` | `main`에서 만들고, 체크포인트 동안 후보 구현을 검토·통합한다. |
| 개인 구현 | `team/<name>/iteration-<n>-<topic>` | 해당 Iteration 통합 브랜치에서 분기한다. 설계·코드·검증 근거를 함께 남긴다. |
| 긴급 수정 | `fix/<short-topic>` | 이미 `main`에 통합된 회귀만 최소 수정한다. |

현재 통합 브랜치는 `iteration/1-tcp-http-response`다. Iteration 1이 끝나면 이 브랜치를 `main`에 병합하고, 다음 통합 브랜치는 갱신된 `main`에서 시작한다.

## Iteration 1 — TCP 기반 HTTP 응답 (#1)

### 목표

클라이언트가 지정 포트에 TCP로 연결하고, 최소 요청을 보낸 뒤 유효한 고정 HTTP 응답을 받는다.

### 이번 단계의 최소 범위

1. CLI 인자로 포트를 받아 서버 소켓을 생성한다.
2. `socket → bind → listen → accept` 순서로 하나의 연결을 수락한다.
3. `recv`로 최소 요청을 읽고, `send` 반복으로 부분 송신까지 처리해 고정 응답을 전송한다.
4. 각 연결의 클라이언트 FD를 닫는다. 단일 연결 순차 처리는 허용한다.
5. `Makefile`로 빌드 가능해야 한다.

응답 기준은 아래처럼 `Content-Length`와 `Connection: close`를 포함한 HTTP/1.1 메시지다.

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 5
Connection: close

Hello
```

### 아직 하지 않는 일

- HTTP 요청 전체 파싱, GET/POST 분기, persistent connection
- 정적 파일 서빙
- Reverse Proxy, Forward Proxy, HTTPS CONNECT
- 동시성

### 후속 단계를 위한 최소 설계 경계

- 요청/연결 상태를 전역 공유 버퍼에 두지 않는다.
- 송신은 C 문자열 종료가 아니라 바이트 길이를 기준으로 한다.
- FD의 소유자와 종료 지점을 함수 단위로 명확히 한다.

### 공통 검증 기준

```sh
make
./server 8080
curl -i http://127.0.0.1:8080/
```

확인할 항목은 `200 OK`, 정확한 헤더와 본문, 반복 연결 후 정상 응답, 클라이언트 조기 종료, 이미 사용 중인 포트의 bind 실패다.

## 체크포인트 산출물

각 개인 브랜치에서 다음을 제시한다.

1. 실행 가능한 코드와 빌드 명령
2. 공통 테스트와 추가 반례의 결과
3. `send` 부분 송신, 오류, FD 수명에 대한 설계 선택과 한계
4. Iteration 2 및 5에서 구조를 바꿔야 할 가능성

통합 브랜치에서는 모든 후보가 DoD를 충족하는지 먼저 확인한 후, 가장 단순하고 검증 근거가 명확한 구현을 기준으로 삼는다. 다른 후보의 개선은 회귀 테스트를 추가한 뒤에만 선택적으로 반영한다.
