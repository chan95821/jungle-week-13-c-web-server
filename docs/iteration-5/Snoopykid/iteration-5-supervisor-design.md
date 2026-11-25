# Iteration 5 부모 감독 루프 통합 — 확정 사항과 다음 선택

2026-11-25. 연결 장부·CLOSED 단위 다음에 실제 부모/worker 실행 흐름을 연결한다. 아래 선택은 아직 제안이며 코드에 적용하지 않았다.

## 기존 확정 — 재선택하지 않음

부모 accept+IPC 전달, 고정 프로세스 worker와 worker별 nonblocking LT epoll, 공유 mmap+robust mutex, worker 직접 backend 선택, W4/Q128/C256/G1152, bounded CLOSED+긴급 pipe, 최소 handler+epoll_pwait 및 반환 전달 구간은 유지한다. worker 하나의 사망 후 남은 worker 계속 서비스와 신뢰 불가 공유 상태 시 전체 종료도 기존 결정이다. 자동 재시작·재시도·백엔드 제외는 아직 허용되지 않았다.

## main부터 통합 순서

1. main: 기존 CLI/config 검증 → 시그널 설정 → listener와 고정 공유/부모 장부/FIFO/채널 준비 → 고정 worker fork.
2. 자식: 다른 worker 채널·부모 listener·긴급 pipe 읽기 사본을 닫고 자기 채널·긴급 쓰기 끝만 유지 → worker LT epoll.
3. 부모: 자식용 채널 끝·긴급 쓰기 끝을 닫고 listener/각 worker 채널/긴급 읽기 끝을 LT epoll에 등록.
4. 반복마다 종료/fatal·waitpid 결과를 먼저 확인 → accept gate → FIFO dispatch → 부분 FD 송신·CLOSED 수신.
5. worker 사망: 해당 세대 배정 중단 → waitpid 확인 → 부모 sender/채널 정리 → 연결 장부와 backend 예약 회수. 공유 mutex EBUSY면 부모 이벤트 루프를 잠그지 않고 후속 반복에 재시도한다. 재시도 중인 사망 세대는 배정/재사용하지 않는다.
6. shared fatal 또는 종료 요청: accept 중단 → FIFO 원본 정리 → worker 종료 요청 → 회수 완료 후 mutex/공유 mapping 파괴.

이는 목표 호출 순서이며 실제 main은 현재 순차 처리다. worker HTTP/프록시/CGI 상태 머신, 파일 I/O 정책, 분배 정책의 시간/점수와 입력 방식도 잔여 단위다. 부모부터 연결하더라도 blocking handle_client를 worker 이벤트 callback에 넣지 않는다.

## 새 선택 — 제어 채널 오류와 종료 상한

### 1 추천: 해당 worker 격리 + 제한시간 종료

- 정상 형식으로 판별할 수 없는 CLOSED/FD 제어 채널, 예상치 않은 단절은 해당 worker를 새 배정 대상에서 제외하고 종료시킨다. shared fatal이 없고 다른 worker 상태가 유효하면 나머지는 기존 결정대로 계속 서비스한다.
- 해당 worker에게 SIGTERM을 보내고 단조 시계 기준 임시 2초 동안 비동기 waitpid(WNOHANG)로 기다린다. 살아 있으면 SIGKILL을 한 번 보내며 실제 회수 뒤에만 그 세대의 장부·예약을 회수한다. 이때 완료 중 응답은 보장하지 않는다.
- 일반 SIGINT/SIGTERM 또는 shared fatal에 따른 전체 종료도 동일한 임시 2초 유예와 SIGKILL 규칙을 적용한다. 이는 요청 전체 타임아웃과 다른 종료 유예값이다.
- 자동 대체 worker는 만들지 않는다. 유효 worker가 하나도 남지 않으면 accept를 중단하고 전체 정리 후 오류 종료한다.
- SIGKILL은 즉시 프로세스 회수를 보장하지 않는다. 특히 커널의 중단 불가 대기에서는 2초를 전체 종료 완료 보장으로 표현하지 않는다. 부모는 waitpid 확인 전 공유 영역을 해제하지 않는다.
- 비용: worker별 종료 상태·기한, 가장 가까운 기한 기반 epoll 대기, 사망 회수 pending 관리. 정상 요청 경로에 새 IPC/잠금을 추가하지 않는다. 성능은 측정하지 않는다.

### 2: 제어 채널 오류도 전체 종료

부분 채널 오류 시 다른 worker도 함께 중단한다. 일부 서비스 지속보다 전체 재시작 가능한 단순한 오류 경계를 우선하는 변경안이다. 기존 ‘worker 사망 후 계속 운영’의 적용 범위와 다르므로 자동 채택하지 않는다. 종료 유예/강제 종료 규칙은 여전히 필요하다.

로그 sink가 막힐 때 처리, 부모 사망 시 worker 수명, 자동 재시작은 이번 묶음에 포함하지 않는다. shared fatal의 제한된 숫자 진단 유지 결정은 그대로지만 실제 로그 sink 처리까지 완료됐다고 하지 않는다.

## 선택 후 검증 후보

- 한 worker 채널 오류/단절과 다른 worker 정상 연결의 동시 진전.
- 종료 요청을 무시하는 테스트 자식에서 2초 종료 기한·SIGKILL·waitpid 순서, 신호와 SIGCHLD 병합.
- FD 일부 전달/완료·CLOSED 적체 중 종료, 해당 세대 회수 한 번, 다른 worker 카운터 유지.
- 모든 worker 사망 후 accept 중단·오류 종료, respawn/재전달 없음.
- shared mutex EBUSY 중 다른 제어·종료 이벤트 진전, 회수 완료 전 mapping 파괴 없음.
- 실제 HTTP worker 통합 전에는 프로세스/IPC fixture 검증과 서버 E2E를 구분.

2초는 미측정 임시 제안이며 성능 측정·부하 실행 허락 요청이 아니다. 사용자의 선택 후 구현 흐름별 상세 테스트를 먼저 확정한다.

## 감독 정책 1번 확정 (2026-11-25)
채널 오류 worker만 배정 중단·SIGTERM, 임시2000ms 후 살아 있으면 SIGKILL 한 번, waitpid 확인 후 sender/채널·연결 장부·backend 예약 회수. 나머지는 계속 서비스하며 자동대체 없음. 모든 유효worker소실은 전체 오류 종료. 사용자 종료/shared fatal도 같은 종료 유예 적용. 신뢰 불가 shared상태면 전체 종료하되 실제 자식 모두 회수 전 mutex/mapping을 파괴하지 않는다.
첫 구현은 부모 감독 상태와 OS kill/waitpid/회수 연결, 실제 fork/epoll fixture다. main/HTTP worker 상태머신·로그sink·부모사망·분배점수는 미통합. 공유mutex EBUSY는 pending으로 반환하고 다른worker/종료처리를 계속한다. 실제 부모루프의 재회수 깨우기/간격은 아직 자동 선택하지 않는다.

## 부모 감독 단위 완료 (2026-11-25)
parent_worker_supervisor.h/c에 PID별 WNOHANG·격리·임시2000ms 유예/TERM/KILL 각1회·waitpid 확인 후 sender/채널→연결장부→backend 원장 회수를 구현했다. shared EBUSY는pending, 신뢰불가면전체종료, 유효worker0 오류종료, 자동대체없음. 첫숫자진단고정보존·ECHILD시PID신호/파괴차단. 일반모듈O2와make all성공, 실제fork/신호/잠금경합 포함ASan/UBSan make test16개통과. W4추가560bytes(현재ABI), 성능미측정. 상세 iteration-5-supervisor-review.md. main/실제HTTP worker·log sink·부모사망·정책점수·EBUSY 재시도 wake/간격은미통합/미결정이며 테스트의20ms를운영정책으로채택하지않음. make verify/부하/커밋/머지/푸시없음.
