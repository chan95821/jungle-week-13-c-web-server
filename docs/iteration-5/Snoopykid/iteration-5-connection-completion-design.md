# 부모 연결 장부와 CLOSED 통지 — 다음 구현 단위

2026-11-25. 이 문서는 구현 전 제안이다. 사용자가 아래 A/B를 선택하기 전에는 제품 코드·프로토콜·오류 정책을 변경하지 않는다.

## 확정 사항

부모는 accept와 worker 관리·FD 전달을 담당한다. worker는 연결 수신·HTTP·백엔드 중계·연결 종료를 담당한다. 일반 CLOSED 통지는 기존 worker별 full-duplex SOCK_STREAM의 worker→parent 방향을 사용한다. 별도 ACK는 추가하지 않는다. 긴급 알림 pipe는 일반 CLOSED 전달에 사용하지 않는다.

부모 FIFO 미배정 연결, worker에 배정 중인 SENDING 연결, worker가 보유하는 연결을 모두 전역 accepted 미종료 수에 정확히 한 번 포함한다. worker outstanding은 배정 후 아직 해제되지 않은 연결 수다. 초기 W4/Q128/C256/G1152는 기존 결정을 유지한다. 백엔드 요청 원장은 이 연결 장부와 구분한다.

## main부터 이어질 흐름

1. main이 parent 전용 고정 연결 장부 G칸, FIFO, worker별 전송 상태를 시작 시 소유한다. 장부에는 본문·헤더·URI를 넣지 않는다.
2. parent가 accept에 성공하면 식별 토큰과 accept 시각을 등록하고 FIFO에 넣는다. 실패 경로는 새 FD를 한 번 닫고 등록을 되돌린다.
3. worker 선택은 별도 선택 로직의 결과를 받는다. 장부를 QUEUED→SENDING으로 옮기며 worker 기여분을 증가시킨 뒤 기존 prepare_worker_fd_transfer/advance_worker_fd_send를 사용한다. 부모 원본 FD는 기존처럼 전체 프레임 송신 완료에만 닫힌다.
4. worker는 전체 FD 프레임 검증 후 연결을 소유한다. 연결 I/O·자원 정리가 끝나면 같은 식별 토큰의 CLOSED 통지를 준비한다.
5. parent가 완전한 CLOSED 프레임을 검증해 정확히 한 번 장부·worker 기여분·전역 수를 해제한다. 부분 수신만으로 해제하지 않는다. 늦거나 중복된 이전 세대 통지는 현재 연결을 감소시키지 않는다.
6. CLOSED가 FD 송신 완료 처리보다 먼저 관측될 수 있다. SENDING 상태에 종료 표시를 보존하고 부모 송신 상태 정리가 완료된 뒤 슬롯을 재사용한다. 이 순서를 시험하며 빠른 worker가 항상 부모보다 늦게 끝난다고 가정하지 않는다.
7. SIGCHLD 후 waitpid로 worker 사망을 확인하면 해당 세대의 연결을 회수한다. 기존 backend 원장 회수는 별도 호출한다. 사망 확인 전에 FD를 다른 worker로 재전달하지 않는다.
8. 공간이 돌아오면 accept gate를 다시 계산한다. 종료/fatal이면 공간이 있어도 accept하지 않는다.

이 흐름은 앞으로 구현할 책임 순서다. 현재 main에는 아직 반영되지 않았다. 함수명·인자 표현은 구현 시 이 책임에 맞춰 정하며 알고리즘 최종 선택은 포함하지 않는다.

## 새 선택: 종료 통지 적체 시 처리

### A 추천 — 제한된 CLOSED 큐 + 보수적 부모 장부

worker별 CLOSED 대기열을 C칸으로 제한한다. 한 통지 프레임만 부분 송신하며 프레임끼리 섞지 않는다. 부분 송신/EAGAIN이면 EPOLLOUT에서 이어 보내고, 대기열이 가득 차면 새 FD 프레임 수신을 보류한다. 기존 연결·백엔드·긴급 제어·종료 처리는 계속 진행한다. 큐 포화를 busy loop나 무제한 할당으로 처리하지 않는다.

parent는 실제 worker close와 CLOSED 수신 사이에도 연결을 보수적으로 점유 중으로 센다. 이는 물리적으로 열린 소켓 개수와 다를 수 있으며, 통지 지연 시 신규 수락·배정이 늦어진다. 대신 아직 통지되지 않은 연결을 중복 배정 여유로 사용하지 않아 기존 C/G 상한과 회수 근거를 보존한다. CLOSED 큐는 연결당 하나의 미통지 완료만 허용하며, C 상한 논리는 부모의 미해제 배정 수로 검증한다.

토큰은 슬롯 식별자와 재사용 세대를 포함한다. 재사용으로 중복·늦은 CLOSED가 다른 연결을 해제하지 못하게 한다. 토큰 wrap은 재사용하지 않고 오류로 보고하는 안이다. 정상 중복·이전 세대는 무변경, 형식 오류·미래 세대·다른 worker 소유권 모순은 제어 채널 오류로 보고하고 정상 배정을 중단한다. 오류의 최종 worker/전체 종료 범위는 부모 감독 흐름과 함께 확정하며 이 단위가 자동 재시작·우회를 추가하지 않는다.

장점: 상한·부분 I/O·정확한 1회 해제를 한 흐름으로 검증할 수 있고 기존 STREAM을 유지한다. 비용: 부모 장부, worker CLOSED 대기열, 부분 프레임 상태 및 통지 지연에 따른 보수적 용량 점유.

### B — 공유 메모리에 연결 완료 상태 추가

worker가 공유 연결 원장을 갱신하고 STREAM은 깨우기 역할로 줄인다. 큐 적체와 별개로 완료 상태를 확인할 수 있지만, 현재 backend 공유 원장 외에 연결 원장의 잠금·부분 갱신·소유자 사망·재복구를 설계해야 한다. 부모의 대기 진입과 통지 유실 경합도 별도로 처리해야 한다. 공유 범위와 복구 비용이 늘어난다.

이번 A/B는 worker/backend 선택 알고리즘이나 통계 구간의 결정이 아니다.

## 메모리 비용 예상과 검증 후보

A의 parent 장부는 G개 고정 엔트리, CLOSED 큐는 worker별 C개 고정 토큰으로 제한한다. 예를 들어 장부 엔트리 48바이트, 완료 토큰 24바이트라면 G1152에서 55,296바이트, W4×C256에서 24,576바이트다. 합계 약 78KiB이며 ABI·최종 필드 확정 전 추정이다. FIFO 4,128바이트, 기존 공유 mapping 33,416바이트, 채널 상태·프로세스·소켓 버퍼는 별도다. 구현 후 실제 sizeof와 총액을 기록한다. 성능 우위는 측정하지 않았다.

선택 후 구현 전 다음 후보를 tests 문서에 확정한다.
- QUEUED→SENDING→OWNED→CLOSED에서 전역 수가 중복 증가하지 않음.
- CLOSED 부분 전송·동반 프레임·EINTR/EAGAIN, 큐 C 경계, 새 FD 수신 보류·재개.
- 부모가 FD 전송 완료를 처리하기 전 worker 종료 통지를 받은 순서.
- 중복 CLOSED, 재사용 슬롯의 이전 세대, 미래/잘못된 토큰 구분.
- worker 사망의 전송 전/중/후·CLOSED 전/중/후 회수, 같은 세대 반복 회수 무변경.
- 다른 worker 연결·세대·카운터 오염 없음, 전역 수=FIFO+배정 미해제 수.
- 포화 시 신규 accept 보류, 기존 제어/종료 처리는 진전, 회수 후 재개.
- 정상·오류·종료에서 실제 FD 개수 복원.

컴파일과 단위 장애 검증을 구분한다. main 통합 서버 E2E와 부하·성능 측정은 이번 문서 작성으로 허용되거나 실행되지 않는다. worker 교체·재시작, 정책 점수/시간 통계, CGI/파일 I/O의 이벤트 루프 전환은 남은 별도 단위다.

## 사용자 A(1번) 확정 — 부모 장부와 bounded CLOSED 큐 (2026-11-25)
부모 전용 G칸 연결 장부와 worker별 C칸 완료 큐를 채택했다. parent는 완전한 CLOSED 전까지 보수적으로 점유 수를 유지한다. sender 정리가 끝나기 전 CLOSED 도착은 표시만 남겨 슬롯 재사용을 막는다. 장부는 FD를 소유하지 않고 FIFO/FD sender/worker가 기존 소유권대로 정리한다. 사망 회수는 waitpid 확인 및 부모 sender abort 후 호출한다. 토큰은 기존 64bit connection_id에 32bit 재사용 세대와 32bit 슬롯+1을 담는다. 세대 wrap은 오류다. 완료 프레임은 고정 32byte로 worker/세대/connection_id를 검증한다. 중복·이전 세대 무변경, 미래·다른 소유권·잘못된 형식은 EPROTO 보고. 종료 범위/자동재시작은 이 단위에서 정하지 않는다.
코드 순서: create_parent_connection_ledger → register_parent_connection → assign_parent_connection → finish_parent_connection_handoff / apply_worker_connection_closed → reclaim_reaped_worker_connections. worker는 enqueue_worker_connection_closed → advance_worker_closed_send, parent는 advance_worker_closed_receive → apply. 큐 full이면 caller가 새 FD 수신을 보류하고 기존 I/O·제어를 계속한다. 이 모듈이 실제 epoll interest/main을 변경하지 않는다.

## 연결 장부·CLOSED 단위 완료 (2026-11-25)
사용자 1번을 parent_connection_ledger.h/c 및 worker_closed_channel.h/c로 구현했다. QUEUED/SENDING/OWNED와 early CLOSED, 토큰 재사용 세대, 정확1회 해제·waitpid 이후 회수, bounded worker 큐와32byte STREAM 부분 I/O를 제공한다. 장부/완료 큐는 FD를 소유하지 않는다. 실제 W4/C256/G1152 경계·부분 I/O/실제EAGAIN·비정상 ancillary·3경계 SIGKILL 회수 포함 ASan/UBSan make test 15개 통과. 일반O2 컴파일 성공. 장부+완료큐+수신 상태62,104bytes(현재ABI), 다른서버자원 제외. main/실제epoll/서버E2E·성능 미통합/미실행. 상세 iteration-5-connection-completion-review.md. 이전 ‘제안/미선택’ 기록은 이 선택과 결과로 대체한다.
