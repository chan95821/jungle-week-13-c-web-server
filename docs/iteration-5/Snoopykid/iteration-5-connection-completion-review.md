# Iteration 5 부모 연결 장부·CLOSED 통지 구현 리뷰

2026-11-25. 사용자 1번(제한된 CLOSED 큐+부모 보수적 장부) 선택을 구현했다. 현재는 독립 구성요소와 조합 fixture까지 완료했으며 제품 main/worker 이벤트 루프는 아직 미통합이다. 기존 서버는 순차 처리다.

## main에서 사용할 함수·데이터 흐름

1. `create_parent_connection_ledger(G,W,C)`는 부모 전용 고정 장부와 worker 카운터를 할당한다. `activate_parent_connection_worker()`는 초기 worker 세대를 등록한다. 재활성화는 사망 회수 후에만 가능하며 자동 재시작을 수행하지 않는다.
2. accept 성공 후 `register_parent_connection()`이 QUEUED 연결을 등록해 전역 수를 증가시킨다. 기존 `enqueue_parent_client()`가 FD를 소유한다. enqueue 실패 시 caller가 FD 정리 및 `cancel_queued_parent_connection()`을 호출해야 한다. 장부 자체는 FD를 보유하지 않는다.
3. 기존 FIFO에서 꺼낸 연결에 `assign_parent_connection()`을 적용하면 QUEUED→SENDING이 되고 worker outstanding이 증가한다. worker 선택 알고리즘을 호출하지 않으며 caller의 선택 결과를 받는다. C 포화는 EAGAIN이며 다른 worker로 자동 재배정하지 않는다.
4. `prepare_worker_fd_transfer()`와 `advance_worker_fd_send()`는 기존처럼 전체 프레임 송신 완료 시 부모 원본 FD를 닫는다. caller는 그 정리가 끝난 뒤 `finish_parent_connection_handoff()`를 호출한다. 토큰은 기존 metadata.connection_id에 그대로 담는다.
5. worker가 연결 FD와 관련 자원을 정리한 뒤 `enqueue_worker_connection_closed()`에 토큰을 넣는다. `advance_worker_closed_send()`는 기존 STREAM 역방향에 고정 32바이트 완료 프레임을 최대 한 번의 nonblocking syscall로 보낸다. head 부분 송신을 마치기 전에 다음 프레임을 시작하지 않는다. EINTR/EAGAIN은 PENDING이다.
6. parent의 `advance_worker_closed_receive()`는 완전한 한 프레임을 검증해 출력한다. `apply_worker_connection_closed()`가 소유권·세대·토큰을 확인하고 카운터를 한 번 해제한다. CLOSED가 부모의 handoff 완료 처리보다 먼저 오면 SENDING에 표시만 남긴다. 이때 total/outstanding과 슬롯을 유지하고 finish 시 해제한다.
7. worker 사망 시 caller가 waitpid로 종료를 확인하고 해당 sender를 abort한 뒤 `reclaim_reaped_worker_connections()`를 호출한다. 같은 세대의 미해제 연결만 회수한다. 반복 회수·늦은 CLOSED는 무변경이며 다른 worker는 유지한다. backend 요청 원장 회수는 별도 책임이다.
8. caller가 전역 수와 FIFO 용량으로 `parent_accept_has_capacity()`를 재평가한다. worker의 `worker_closed_has_capacity()`가 false면 새 FD 프레임 수신을 보류하되 기존 클라이언트·백엔드·긴급 제어·종료 처리는 계속해야 한다. 실제 epoll interest 변경은 아직 구현하지 않았다.

## 상태·소유권·오류 계약

연결 토큰은 기존 uint64 connection_id의 상위 32비트 재사용 세대, 하위 32비트 슬롯+1이다. 초기 세대는 1, 재사용 시 증가하며 wrap은 EOVERFLOW로 거부한다. 해제한 슬롯에도 tombstone을 유지한다. 이전 토큰·중복 완료는 무변경, 현재 토큰의 잘못된 worker·미래 세대·형식 모순은 오류다.

parent 장부는 FD를 닫지 않는다. FD 소유자는 FIFO → 부모 sender/worker receiver → worker 연결 흐름으로 유지한다. ledger destroy가 실제 연결 정리를 대신하지 않는다. worker CLOSED 큐도 FD를 소유하지 않는다.

CLOSED 큐의 capacity에는 부분 송신 head도 포함한다. 미송신 통지는 부모의 보수적 미해제 배정 수에 포함되므로, 정상 caller가 C 상한을 지키면 완료 큐와 살아 있는 배정의 합도 그 상한 안에 남는다. helper의 gate만으로 운영 worker 입장 제어가 완성되는 것은 아니다. enqueue 실패 시 토큰을 버리면 안 되며 통합 caller가 오류를 처리해야 한다.

미완료/잘못된 프레임 후 resync하지 않는다. 예상치 않은 SCM_RIGHTS는 수신 FD를 한 번씩 닫고 EPROTO로 보고한다. 제어 채널 EOF/reset/오류 후 supervisor의 worker·전체 종료 범위는 별도 결정이다. 자동 재시도·우회·재시작은 추가하지 않았다.

## 컴파일과 실제 검증

- 일반 C11/Wall/Wextra/Werror/O2, 테스트 훅 없는 두 모듈 컴파일: 성공.
- ASan/UBSan `make test`: 15개 실행 파일 모두 성공(exit 0).
- 장부 total=queued+assigned 및 worker 기여분 일치, 실제 W4/C256/G1152 상한: 통과.
- SENDING 중 CLOSED 선도착, 중복/이전·미래 세대·잘못된 소유권, 토큰 wrap: 통과.
- bounded ring full/재사용, 부분 send/recv, EINTR, 실제 소켓 buffer EAGAIN·재개, EPIPE: 통과.
- 동반 완료 프레임 순서, 7개 잘못된 프레임, 예상치 않은 1/32개 FD와 ancillary truncation 정리, EOF 0/1/31바이트: 통과.
- 실제 FIFO→SCM_RIGHTS 인계→역방향 CLOSED→보수적 capacity 회수 조합: 통과. 이 조합은 socketpair 기반이며 TCP accept/main E2E가 아니다.
- 실제 자식 SIGKILL: FD 미수신/CLOSED 부분 송신/송신 완료 세 경우에서 waitpid 확인 후 정확히 한 번 회수, 다른 worker 유지, 늦은 통지/새 세대 오염 방지: 통과.
- 정상·오류·사망 시험 전후 FD 개수 일치: 통과.

첫 사망 시험은 ‘FD를 읽지 않은 자식 종료는 반드시 EOF’라는 테스트 가정 때문에 실패했다. 실제 UNIX STREAM은 unread FD frame이 있으면 ECONNRESET도 반환할 수 있었다. EOF 또는 해당 reset을 제어 채널 종료로 인식한 후 동일 회수를 확인하도록 fixture 기대값을 수정했고 재실행 통과했다. 제품 구현은 reset을 이미 오류로 보고했다.

최종 로그: `build/iteration5-capability/unit-tests-with-connection-completion.log`. 단독 이전 로그는 당시 결과를 담으므로 최신 전체 로그를 기준으로 한다.

## 실제 저장 공간과 비용

현재 WSL ABI의 sizeof:

| 항목 | 바이트 |
| --- | ---: |
| parent 연결 엔트리 | 32 |
| parent worker 카운터/세대 | 24 |
| ledger owner | 56 |
| CLOSED 토큰 | 24 |
| worker 큐 owner(부분 frame 포함) | 80 |
| parent CLOSED receiver | 48 |

G1152/W4/C256 기준 부모 장부는 32×1152+24×4+56=37,016바이트다. worker 완료 큐는 4×(24×256+80)=24,896바이트이며 부모 수신 상태는 48×4=192바이트다. 합계 62,104바이트(약 60.6KiB), 프로세스들에 나뉜 user-space 저장 공간이다. 기존 FIFO4,128·공유mapping33,416·sender/receiver·epoll·HTTP·CGI·소켓 kernel buffer는 제외한다.

정상 등록은 빈 슬롯 탐색 O(G), 완료 적용은 토큰 직접 조회 O(1), 사망 회수는 O(G), 큐 중복 확인은 O(C), 명시적 전체 검사는 O(G×W)다. 생성 뒤 처리 경로에서 메모리를 확장하지 않고 공유 잠금·파일/네트워크 I/O를 장부 내부에서 수행하지 않는다. 성능·부하 측정 결과가 아니다.

## 남은 통합·미검증

실제 main·부모/worker LT 이벤트 루프·epoll 보류/재개, supervisor 종료 정책, 실제 CGI/파일 I/O 전환, 네 선택 정책의 시간·점수·입력 규칙은 남아 있다. make verify 서버 E2E, 광범위한 메모리/할당 실패 주입, 부하·성능은 이번 실행 범위에서 제외했다. 기존 HTTP·라우팅·타임아웃은 변경하지 않았다. 커밋·머지·푸시하지 않았다.
