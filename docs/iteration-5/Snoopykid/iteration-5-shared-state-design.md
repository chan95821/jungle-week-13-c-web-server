# 공유 분배 상태와 처리 흐름 — 확정/제안 구분

2026-11-25. 사용자 결정과 미결정 설계 초안. 제품 코드 미구현, 기능·부하·성능 실행 승인 아님.

## 확정
- 부모: accept, IPC 클라이언트 FD 전달, 작업자 관리·회수. 요청마다 백엔드를 대신 선택하지 않는다.
- 작업자: 수신·파싱·라우팅, 공유 상태를 통한 백엔드 선택, 연결·중계·응답, 완료·실패 상태 갱신.
- 공유 메모리 + 작업자 직접 선택. 선택과 진행 요청 예약 증가는 한 동기화 구간. 네트워크·파일 I/O는 잠금 밖.
- RR / Least Outstanding+동률 RR / 시간 기반+동률 RR / 진행 수·시간 결합+동률 RR 네 정책을 같은 상태 구조로 선택 실행하는 것은 허용. 최종 정책과 구체 공식은 미결정. 범용 플러그인 프레임워크를 만들지 않는다.
- HTTP·라우팅·기존 오류 유지. 자동 재시도·장애 백엔드 제외·정책 자동 전환 추가 금지.

## main부터의 흐름 (직접 구현할 함수명의 제안, 기존 API 아님)
1. main: 기존 설정·public·시그널 검증, 동시성/정책 설정 검증, create_shared_backend_state, 리스너와 작업자 IPC 준비, fork.
2. 부모 run_parent_loop: listener/IPC/자식 종료 이벤트 관리. accept한 FD와 accept 시각·연결 ID를 dispatch_client_connection으로 전달. 부모는 HTTP 파싱·백엔드 선택을 하지 않는다.
3. 자식 run_worker_loop: 상속한 리스너와 타 작업자의 IPC 끝을 닫고 자기 epoll/IPC를 관리. receive_client_connection이 연결 상태를 생성한다.
4. advance_client_request: 헤더/본문 부분 수신, 기존 http_parse_request → http_select_route → proxy_validate_request 순서와 Expect/오류 우선순위 보존. 요청 버퍼는 작업자 내 연결이 소유.
5. 로컬: 메모리/정적/CGI 응답 단계로 이동. 파일 I/O·CGI 블로킹 처리 방식은 미결정이며 기존 blocking handler를 그대로 루프에서 호출하지 않는다.
6. 프록시: reserve_backend_for_request가 공유 잠금을 얻어 정책 실행·요청 예약 기록·진행 수를 함께 갱신한다. 제출 목적지는 고정, 조회는 선택 정책 적용. 잠금 해제 후 start_backend_connection.
7. advance_backend_connection: non-blocking 연결 완료 확인, 부분 요청 송신, 헤더/본문 수신·클라이언트 부분 송신, 역압력·기한 처리. 최종 응답 이후 두 번째 오류 응답 금지.
8. finalize_backend_request: 완료·실패 시 단 한 번 공유 예약 해제·선택한 통계 규칙 갱신. close_connection은 이벤트 등록·FD·버퍼·예약 수명을 정리. 정확한 예약 종료 시점은 아래 별도 선택.
9. 부모 reap_worker: waitpid로 사망 확인 후 해당 작업자 세대의 예약을 회수한다. 부모가 백엔드를 선택하지 않는 원칙과 모순되지 않는다. 재시작 여부·속도·실패 정책은 미결정.
10. 종료: 부모 accept 중단 → 작업자 종료·회수 → IPC/리스너 종료 → 모두 접근 종료 후 공유 mutex 파괴·munmap. 부모 사망 대응·강제 종료 기한은 별도 선택.

## FD와 메모리 소유권 제안 (승인 전)
- 부모 listener 단독 소유. 자식은 fork 직후 상속 사본 close.
- IPC 양끝은 해당 프로세스가 소유. 클라이언트 FD는 SCM_RIGHTS 성공 시 커널에 전달 참조가 생기므로 부모 사본 close, 작업자 recvmsg 이후 작업자 사본 소유. 전송 EAGAIN은 부모의 제한 큐에 남김, 실패는 부모 정리. 큐 상한·전달 확인·누락/중복·IPC 종료와 아직 recv하지 않은 FD 처리는 별도 설계.
- 백엔드 FD·정적 파일 FD·CGI pipe/자식 추적은 담당 작업자의 연결이 소유. shared state에는 FD나 프로세스 내부 포인터를 넣지 않고 인덱스·세대·ID를 둔다.
- 공유 매핑 생성·초기화·최종 해제 책임은 부모. mutex 사용은 부모 회수와 작업자 선택/완료. 부모는 선택 알고리즘을 실행하지 않는다.
- 현재 router_response/body_provider의 자기 참조 수명은 유지하거나 전환 계약을 합의한다. 상태 객체를 임의 이동·재할당하지 않는다.

## 선택 1: 공유 메모리와 동기화 (기록 당시 제안; 아래 추가 결정 우선)
추천 후보: fork 전 MAP_SHARED|MAP_ANONYMOUS mmap + process-shared robust pthread mutex 하나. Linux/WSL 대상, epoll도 Linux 전용. pthread를 사용해도 작업자 모델이 스레드 풀로 바뀌지는 않는다.
대안: shm_open/ftruncate/mmap(MAP_SHARED), 생성 후 shm_unlink·FD 정리. 이름·권한·실패 정리가 늘며 POSIX 공유 메모리 인터페이스를 사용하지만 robust mutex/epoll까지 다른 OS 지원이 자동 보장되지는 않는다.
mutex 설정·지원 검사 실패 시 시작 중단 제안. private mutex로 조용히 대체하지 않는다. 플랫폼 미지원 오류 정책은 채택 필요.
이벤트 루프에서 blocking mutex_lock 대기를 피하려면 trylock 실패 시 현재 연결만 보류하고 다른 이벤트를 처리하는 방식 제안. 재개 타이머/알림·기한·공정성·혼잡 정책 미결정. busy-spin하지 않는다. 단일 mutex는 여러 상태 잠금 순서보다 단순하지만 경쟁이 생길 수 있으며 성능 우위는 미측정.

## 선택 2: 소유자 사망 복구 (기록 당시 제안; 아래 추가 결정 우선)
robust mutex의 EOWNERDEAD는 데이터 자동 복구가 아니다. 정합성 복원 후에만 pthread_mutex_consistent 호출.
A 제안: 상태 변경 전 복구 기록을 만들고 변경을 commit하는 제한된 트랜잭션. 죽은 소유자의 미완료 변경은 이전 값으로 복원한 뒤 예약 원장에서 집계 재구성. 정상 commit은 보존. 선택 순번·시간 통계·예약도 일관성 대상이며 카운터만 다시 합산해서 충분하다고 하지 않는다. 공유 commit 표시와 메모리 가시성·원자성·복구 도중 재사망 처리를 구현 전 구체화한다.
B 대안: 소유자 사망/복구 불능 시 오류 기록 후 서버 전체를 중단·재시작 대상으로 둠. 자동 데이터 복구 구현은 줄지만 정상 연결도 종료. 둘 다 자동 재시도/백엔드 제외를 의미하지 않는다.
복구 불능 또는 ENOTRECOVERABLE 시 fail-stop은 추천 후보, 아직 확정 아님. 잠금 복구와 사망 작업자 예약 회수는 별도 절차다.

## 선택 3: 기여분과 예약 원장 (미결정)
추천: 작업자 슬롯+세대, 요청 슬롯+세대, backend index, ACTIVE/RELEASED 상태를 공유 원장에 기록. 집계는 백엔드별 전체 outstanding과 작업자·백엔드별 기여분. 원장의 활성 예약으로 집계를 복구할 수 있게 한다. 원장 상한은 동시 연결/진행 요청 상한 확정 후 결정.
작업자 사망은 부모가 waitpid로 확인한 뒤 그 세대의 활성 예약만 회수한다. 재사용한 PID/슬롯에 이전 완료가 적용되지 않게 한다. 중복 완료는 카운터를 다시 감소시키지 않으며 진단 방침은 미결정. 회수 자체도 중간 사망에 안전해야 한다.
대안: 작업자·백엔드별 카운터만 유지. 메모리는 작지만 개별 요청 중복 완료와 중간 갱신 복구를 별도 방식으로 증명해야 한다.

## 나머지 정책 제안과 선택 대기
- 진행 수: 선택·예약을 동시에 시작, backend 응답 프레이밍 정상 완료 또는 실패/취소 시 해제 제안. 느린 클라이언트 역압력은 backend 완료를 늦출 수 있으나 클라이언트 응답 종료까지 무조건 유지하는 정책과 구분. 204/304·EOF/chunked 정상 종료·연결 실패·클라이언트 종료 각각 시점 필요.
- 시간: CLOCK_MONOTONIC, connect 시도 시작~backend 전체 응답 정상 완료 구간을 우선 후보. TTFB 대안도 설명 후 선택. IPC/잠금 대기 제외·backpressure 포함 여부를 명시할 것.
- 평균: EWMA 대안 산술평균/최근 창. 계수·초기 미측정 선택·노후화·재탐색은 숫자까지 별도 선택. 현재 임의값 도입 금지.
- 점수: (outstanding+1)*대표 응답 시간은 후보 예시일 뿐 채택 아님. 단위·수치 범위·표본 부족·동률 허용 오차·RR 순번 갱신 규칙 선택 필요.
- 실패/시간 초과: outstanding은 해제하되 시간 표본 제외/페널티/별도 실패 통계 중 선택. 빠른 실패를 빠른 성공처럼 학습하지 않을 것. 정책은 미결정.
- 입력: 기존 proxy.conf에 명시적 selection 키(rr/least-outstanding/response-time/combined)를 추가하는 안과 CLI 옵션 안 비교 후 선택. 기본값·누락·중복·알 수 없는 값·정책 파라미터 처리 미결정. 기존 설정 파일 필수 계약 유지.

## 검증 후보 (미실행)
동시 선택/완료의 합계 보존, 특정 backend에 대한 경합, 중복 완료·오래된 세대, 선택/예약/commit/완료/복구 각 지점 SIGKILL, waitpid 회수와 정상 완료의 경합, 회수 반복·슬롯 재사용, 복구 실패·지원 API 실패 시 안전 종료, IPC 전달 중 작업자 사망·FD 참조 정리. 정상·지연·비용 차이·장애·회복 비교는 동일 조건. 부하·성능 실행은 조건·지표·작업자 수·기간·반복·환경 제시 후 별도 승인.

## 참고한 API 원문
- https://man7.org/linux/man-pages/man2/mmap.2.html
- https://man7.org/linux/man-pages/man3/shm_open.3.html
- https://man7.org/linux/man-pages/man3/pthread_mutexattr_setpshared.3.html
- https://man7.org/linux/man-pages/man3/pthread_mutex_consistent.3.html
- https://man7.org/linux/man-pages/man7/unix.7.html

## 추가 확정 — 1-A/2-A (2026-11-25)

fork 전 익명 공유 mmap 상속, process-shared robust mutex 사용 및 복구 후 계속 운영 목표를 채택했다. shm_open/독립 프로세스 참여 제외. 요청 원장·기여분·복구 기록의 구체 구조와 복구 불가 시 대응·재시작은 미확정. 다섯 장애 상황·비용·불변 조건·검증 기준·WSL 지원 probe 결과는 iteration-5-recovery-review.md 참조.

## 구조 A와 복구 범위 채택 / 초기화 첫 구현 (2026-11-25)
사용자는 ①~④ 복구·재복구와 상한 있는 원장/복구 기록, ⑤ 제한 진단 로그 및 신뢰 불가 시 전체 안전 종료, 구조 A를 승인했다. 개별 수정 단계·복구 중 종료 주입 검증도 승인했다. 앞선 제안/선택 대기 문구는 이 결정으로 대체하되 통계·알고리즘·재시작·실행 상한 수치는 아직 미결정.
공유 상태 크기 계획/생성/해제와 독립 단위 테스트를 구현했다. 컴파일 성공 및 ASan/UBSan 단위 실행 성공: 예산 경계/초기화/실제 fork 자식 잠금 보유 중 SIGKILL 후 EOWNERDEAD 감지와 mutex 재사용. 실제 원장 갱신 복구가 통과한 것은 아니다. 상세 함수 리뷰·정확 메모리 예시·미검증 범위는 iteration-5-shared-state-initialization.md 참조. main 통합·전체 복구·로그/전체 종료·성능은 미구현/미실행.

## 원장 commit/rollback·재복구 단위 완료 (2026-11-25)
src/backend_reservations.c의 예약/완료·세대·회수·trylock/owner-death 복구와 bounded diagnostic을 구현했다. 고정 undo1개128bytes, header232/worker40/reservation32, scratch24W bytes/프로세스. 메모리 예시·actual 함수 순서·불변 조건·한계는 iteration-5-reservations-review.md 참조.
make test 10개 통과, 추가 commit 보존 경계를 포함한 최종 새 테스트도 통과: 변경8+8·generation6·재복구7+6·회수20·손상6+3·4프로세스 강제경합. 테스트는 실제 fork/SIGKILL이지만 원장 단위이며 TCP 서버 worker E2E가 아니다. main/epoll·부모 전체 종료는 미통합. 정책/시간 통계·실행 상한·재시작·부하/성능 미결정·미실행.
