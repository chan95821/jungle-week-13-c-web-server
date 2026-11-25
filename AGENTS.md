# AGENTS.md

이 저장소는 POSIX 소켓 API만을 사용해 **C 언어로 HTTP 웹 서버 및 Reverse Proxy**를 구현하는 프로젝트입니다.
작업 전 아래 문서들을 먼저 확인하고 범위와 제약 사항을 준수하세요.

## 기준 문서

- **요구사항 문서 폴더**: `docs/requirements/`
  - [웹 서버 만들기 요구사항](docs/requirements/web-server.md) — 과제 목표, 필수 기능, 기술 스택 제한, 이식성, 중점 포인트, 품질 및 테스트 기준
- **개발 진행 맥락**: [docs/DEVELOPMENT_CONTEXT.md](docs/DEVELOPMENT_CONTEXT.md) — Iteration `#1`~`#6` 진행 방식(GitHub Issues 기준), 브랜치 전략, 체크포인트 산출물 및 리뷰 기준

## 핵심 제약 및 원칙

1. **기술 스택 제한**
   - 언어: C (macOS / Linux / WSL2 이식성 고려 — POSIX 표준 범위 또는 `#ifdef` 분기 사용)
   - 허용: 표준 C 라이브러리 및 POSIX API (`<sys/socket.h>`, `<netinet/in.h>`, `<unistd.h>`, `<pthread.h>` 등)
   - **금지**: `libevent`, `libuv`, `nginx`, `Apache`, `mongoose` 등 완성된 HTTP / 이벤트 루프 라이브러리 및 프레임워크
   - 빌드: `Makefile` 또는 `CMakeLists.txt` 제공
2. **프로젝트 범위 (Development Context 우선)**
   - Proxy 구현 범위는 **Reverse Proxy만** 다룹니다 (Forward Proxy, HTTPS `CONNECT` 터널링 제외).
   - 현재 Iteration의 범위와 설계 경계를 지키고, 아직 하지 않는 항목을 앞당겨 복잡도를 높이지 않습니다 (GitHub Issues `#1`~`#6` 및 `docs/DEVELOPMENT_CONTEXT.md` 참고).
3. **구현 및 품질 기준**
   - 요청/연결 상태를 전역 공유 버퍼에 두지 않고, 바이너리 안전(바이트 길이 기준) 송수신과 명확한 FD 소유권/종료 지점을 유지합니다.
   - 메모리 누수·버퍼 오버플로우 방지(`valgrind` 또는 `-fsanitize=address,leak` 검증), `SIGPIPE` 무시 및 `SIGINT`/`SIGTERM` 안전한 종료를 고려합니다.
   - 구현 후 단위 테스트, `curl` 등 실제 클라이언트 기능 테스트, 엣지 케이스 검증을 수행합니다.

## 탑다운(Top-Down) 작업 및 Clean Code 원칙

### 의사결정과 코드 흐름 리뷰

- 사용자가 결정할 사항은 외부 동작, 설계·책임 경계, 자원 소유권·수명, 안전·오류 정책, 성능 및 중요한 트레이드오프다. 선택지와 차이를 설명하고 사용자 결정을 기다린다.
- 함수·변수명, 단순 문법, 이미 합의된 책임과 계약을 표현하는 인자·반환 형태는 기존 코드와 프로젝트 기준에 맞춰 구현자가 처리한다. 단, 겉으로 단순한 시그니처라도 동작·소유권·수명·오류 처리를 바꾼다면 사용자와 먼저 결정한다.
- 세부 표현을 하나씩 승인받기보다 의미 있는 함수 흐름 단위로 작업한다: 변경할 흐름과 기존 결정을 설명 → 합의 범위만 구현 → 실제 코드 순서대로 책임과 데이터·자원 흐름을 함께 리뷰 → 새로운 중요한 선택이 나오면 멈춰 논의한다.
- 컴파일 확인과 기능·회귀 검증을 구분해 보고한다. 검증 범위와 시점은 사용자의 명시적 지시 및 프로젝트 워크플로우를 따른다.

1. **요구사항·설계 우선 (바로 코드 작성 지양)**
   - 구현 전 **요구사항·구현 경계 명확화 → 전체 흐름(`main`) → 하위 함수** 순으로 탑다운 설계를 먼저 맞춥니다.
   - 함수나 시스템 동작을 설명할 때는 **표준 C/POSIX API(원래 있는 함수)**와 **직접 구현·CS:APP 관례 함수**를 명확히 구분하고, OS 책임(바이트 순서·전송 신뢰성)과 서버 책임(메시지 경계·상한·검증)을 분리해 설명합니다.
2. **확장성 있는 설계 ≠ 미래 예측**
   - 현재 Iteration에서 쓰지 않는 훅·콜백·과도한 파일 분리를 미리 만들지 않습니다.
   - 대신 나중에 바꾸기 어렵게 만드는 결합(**전역 공유 버퍼, C 문자열 `\0` 길이 의존, 수신/송신 혼재**)만 피하는 수준으로 단순하게 설계합니다.
3. **단일 책임 및 Clean Code**
   - 연결 조율(`handle_client`)과 수신 프레이밍(`recv_until_header_end`), 완전 송신(`send_all`)처럼 역할이 다른 로직은 작은 함수로 분리합니다.
   - 모든 정상·오류 경로에서 FD 정리 지점을 명확히 유지하고, 엣지 케이스는 원리(미수신/조기 EOF, 분할·동반 도착, 형식 위반, 버퍼 상한) 기준으로 도출합니다.

## Iteration 개발·검증 워크플로우

1. **요구사항·경계 문서화**: 각 Iteration 시작 시 `docs/iteration-<n>/<user>/iteration-<n>-requirements.md`에 In-Scope / Out-of-Scope를 먼저 정리합니다.
2. **테스트 기준 선(先) 누적**: 탑다운으로 함수 설계를 확정할 때마다 테스트 케이스를 `docs/iteration-<n>/<user>/iteration-<n>-tests.md`에 누적(append)한 뒤 구현으로 넘어갑니다.
3. **체크포인트 통합 문서 반영**: 통합 시 채택된 기준 구현에 맞춰 `docs/iteration-<n>/iteration-<n>-requirements.md`와 `docs/iteration-<n>/iteration-<n>-tests.md`를 최종본으로 정리합니다.
4. **목적별 분리 검증**:
   - **단위 테스트 (`make test`, `-fsanitize=address,undefined`)**: E2E로 재현하기 어려운 경계값, `short count` 부분 송신, 분할 수신 집중 검증
   - **E2E 검증 (`make verify`)**: 실제 `./server` 프로세스 기준 `curl`, 반복 연결, 조기 종료/`SIGPIPE`, 포트 점유 실패, `SIGINT` 안전 종료 확인
