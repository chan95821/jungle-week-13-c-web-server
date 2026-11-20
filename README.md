# jungle-week-13-c-web-server

## 빌드·실행

Ubuntu WSL2 셸에서 프로젝트 폴더로 이동한다.

```sh
cd /mnt/c/Users/David/SW-AI-W02-03-TEMPLATE/jungle-week-13-c-web-server
make
./webserver 8080
```

Makefile은 C11, -Wall -Wextra -Wpedantic, -g, -O0를 사용한다. 포트는 숫자로 된 1~65535의 필수 CLI 인자다. 다른 포트도 지정할 수 있다.

서버는 모든 IPv4 로컬 주소인 0.0.0.0에 바인딩한다. 이번 검증은 로컬만 대상으로 하며 외부 PC 접근·방화벽 변경은 포함하지 않는다.

## 현재 동작

- socket → SO_REUSEADDR·호출별 타임아웃 설정 → bind → listen → accept.
- 블로킹·순차 처리. 각 연결은 handle_connection에서 처리하고 main에서 한 번 닫는다.
- 요청 줄·헤더·CRLFCRLF를 합쳐 최대 8192바이트까지 받는다. recv 한 번이 요청 하나라고 가정하지 않는다.
- 제한 안에서 헤더가 끝나면 HTTP/1.1 200 OK와 Hello, world! 및 LF 줄바꿈을 전송한다. 본문은 14바이트이며 Content-Length도 14다.
- Content-Type: text/plain; charset=utf-8, Connection: close. 응답을 전송한 뒤 연결을 닫는다.
- 헤더가 제한 안에 끝나지 않으면 431 응답을 시도하고 종료한다. 메서드·URI·버전·헤더 의미와 POST 본문 파싱은 아직 구현하지 않았다.
- 정상 사용 범위는 본문 없는 요청이다. 읽지 않은 추가 데이터가 남은 연결은 close 시 TCP reset으로 응답이 잘릴 수 있으며 431 응답도 검증 대상이다.
- send는 반환된 길이만큼 진행하며 나머지를 반복해서 보낸다. SIGPIPE는 무시하되 송신 오류는 확인한다.
- SIGINT·SIGTERM 핸들러는 종료 표시만 남긴다. main 흐름에서 연결과 리스너를 정리하며 응답 완료는 보장하지 않는다.

## 임시값과 제한

server.c 위쪽 한 곳에서 관리한다.

| 상수 | 임시값 | 의미 |
|---|---:|---|
| HEADER_LIMIT_BYTES | 8192 | 요청 줄·헤더·끝 경계의 합. 본문 제한 아님. |
| LISTEN_BACKLOG | 128 | 연결 대기 큐 요청값. OS 제한의 영향을 받으며 동시 처리 능력 아님. |
| IO_TIMEOUT_SECONDS | 5 | 수신·송신 호출별 제한. 전체 요청·응답 완료 기한 아님. |

리스닝 소켓에도 동일한 타임아웃을 설정해 신호 검사 직후 accept를 시작하는 경우에도 무기한 대기가 남지 않도록 했다. 호출이 신호로 중단되면 종료 표시를 확인하며, 표시가 없으면 재시도한다.

조금씩 계속 보내는 클라이언트는 현재 호출별 제한을 피하며 서버를 오래 점유할 수 있다. 전체 헤더 기한, keep-alive, 동적 버퍼, 동시성 모델은 추후 별도 결정한다. 최종 동시성 모델을 전제로 전역 연결 상태를 만들지 않는다.

## 학습 연결

CSAPP 11장의 listening descriptor는 새 연결을 받는 listener이고, connected descriptor는 accept가 돌려주는 client다. 수신·송신·close는 연결용 descriptor에 수행한다. TCP가 바이트 스트림을 제공하므로 HTTP 경계 확인과 부분 I/O 반복은 애플리케이션 책임이다.

## 진행 자료

- PLAN.md: 전체 계획, 결정 기록, 구현·검증 상태.
- VERIFICATION.md: Iteration 1 공식 검증 제안과 포함·제외 범위. 아직 실행 전.
- EXPERIMENTS.md: 추후 버퍼·backlog·타임아웃·빌드·동시성 비교 목록. 지금 구현·실행하지 않음.
- AGENTS.md: 사용자 선택 및 검증·성능 측정 절차.
