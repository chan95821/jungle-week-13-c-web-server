# 부모 전달 큐·상한·포화 — 선택안

2026-11-25. 제안/계산만 기록. 이번 단계의 숫자·외부 동작·코드·측정은 아직 채택/실행하지 않았다.

## 기존 확정
parent accept, SOCK_STREAM/SCM_RIGHTS, 전체 frame 전송 뒤 parent 원본 close/noACK, 첫 FD 전송 뒤 자동 재배분 금지. worker outstanding은 배정 후 미종료 연결 수, worker/backend 네 정책 계산 공유·상태/설정 별도. parent는 HTTP 파싱/backend 선택을 하지 않는다.

## 첫 선택: 대기 큐 위치
A 추천: parent의 전역 FIFO에는 아직 worker 미배정 연결만 보관. 전송을 시작할 수 있는 worker를 선택할 때 배정하고 worker outstanding 증가, worker별 active 송신 frame은1개. 부모 큐에서 기다린 연결은 전역 pending으로 별도 집계. 한 worker의 미송신 frame 바이트를 다른 frame과 교차시키지 않는다.
장점: 아직 안 보낸 연결을 특정 worker 큐에 묶지 않음, 전체 큐 상한·원본 FD 예산을 계산하기 쉬움. 비용: worker 정책 선택 시점이 accept 직후가 아니라 실제 dispatch 시점이고, 중앙 큐/배정 단계 관리가 필요. 특정 worker로 이미 시작한 frame은 끝날 때까지 해당 채널에 고정.
B 대안: accept 후 worker를 선택해 worker별 상한 있는 FIFO에 보관, 채널마다 순서대로 전달. 배정된 대기 연결도 worker outstanding에 포함. 장점: worker별 큐가 명확하고 처음 배정을 유지. 비용: worker 하나가 느리면 그 큐의 연결은 대기하고 다른 worker에 여유가 있어도 자동 이동하지 않음. 각 큐 상한과 전체 합계 상한이 모두 필요.
A/B는 backend 분배 알고리즘과 다른 admission/queue 정책이며 같은 네 선택 계산을 호출할 수 있다. 최종 알고리즘은 여전히 미정.

## 다음에 선택할 상한
Q: parent 미전달 queue 용량, W: worker 수, C: worker별 assigned 미종료 연결 상한, G: global accepted 미종료 전체 상한. 상한 관계·설정 입력/필수 여부와 숫자는 미정.
A에서 queue+worker별 active frame만 고려한 parent 보유 client 원본 FD는 최대 Q+W(전체 G로도 제한하는 안). listener/IPC/시그널·자식관리 FD는 별도. frame 전송이 끝난 client는 parent FD에서 빠져도 worker outstanding/전역 연결 장부에서는 종료까지 유지.
메모리: FIFO Q개 엔트리 + worker별 송신 상태 W개 + 전체 연결 장부 G개 + worker 카운터/세대 W개. 각 엔트리에는 FD/connectionID/accept시각/상태만, HTTP 본문/헤더는 넣지 않는 안. 실제 sizeof와 Q/W/C/G 선택 후 총액 기록. shared backend 원장과 parent 연결 장부는 별도이며 숫자를 자동 연동하지 않는다.
미배정 pending과 이미 배정된 ACTIVE/SENDING/worker 처리 상태를 구분하고 한 connectionID는 전역1개만 세는 안. worker 종료/중복 늦은 CLOSED 이벤트로 이중 감소하지 않도록 generation과 장부 확인. 이 구체 장부·입력 숫자는 선택 후 구현.

## 포화 대안 (아직 미정)
1 추천 후보: parent queue/전역 상한이 차면 listener 읽기 감시·accept를 잠시 중단하고 여유 시 재개. 이미 수락한 FD 예산을 더 늘리지 않음. 새 연결은 kernel listen queue에 대기할 수 있고 그마저 차면 연결 지연/실패가 생길 수 있으며 HTTP503을 보장하지 않음. listener backlog와 user queue/실제 동시 처리 능력은 별개.
2 대안: accept 후 즉시 정리/거절. 연결이 끊길 수 있으며 parent가 HTTP를 파싱하지 않는 기존 책임 아래 일괄503 송신을 자동 추가하지 않음. response 정책을 원하면 별도 설계·선택 필요.
상한 확인과 accept 사이의 단일 parent 흐름, EMFILE/ENFILE, queue 저장 실패의 즉시 정리, worker가 전부 unavailable일 때 대응도 선택 후 구체화.

## 시간·취소·안전 종료 미정
accepted_at은 accept 시각 유지. queue에서 대기해도 기존 total header 기한을 worker에서 새로 시작하지 않음. parent 미전달 대기 제한·partial frame 완료 기한·만료 연결 처리·worker channel 오류·미전달 연결의 재배정 여부를 별도 선택.
fatal 제어 메시지/정지 채널·부모 사망/강제 종료 기한은 아직 미정. FD queue 포화가 fatal 통지/전체 종료를 영원히 막아서는 안 됨. 이 문서의 queue 선택이 재시작/재시도/성능 승인 아님.

## 검증 후보
용량0/1/Q 경계, partial sender가 있을 때 다른 worker 채널 진전, queue FIFO·목표worker 세대/정책 선택 시점, assigned/pending/global 합계 보존, worker death·취소/중복 CLOSED 후 정확1회 감소, 실제 IPC EAGAIN·listener 보류/재개, queue/global/FD 상한 도달 후 누수 없는 정리. 실험 fixture/loopback만 사용, throughput/latency 지표 수집 별도 허락.

## API 근거
https://man7.org/linux/man-pages/man2/accept.2.html
https://man7.org/linux/man-pages/man2/listen.2.html

## 전역 FIFO 확정 — 사용자 1번 (2026-11-25)
parent 전역 FIFO에는 worker 미배정 연결만 보관한다. 전송 시작 시 전달 가능한 worker를 공통 정책으로 선택하고 그 worker의 미종료 연결 수를 증가시킨다. worker별 대기 FIFO는 만들지 않는다. channel별 active 송신 frame1개는 기존 STREAM 직렬화 계약을 유지한다.
전역 accepted 미종료 수는 accept 후 관리 장부에 등록할 때 증가하는 안, worker count는 FIFO에서 선택 worker의 SENDING으로 이동할 때 증가한다. FIFO→SENDING→SENT/worker처리 이동은 전역 연결 수를 중복 증가시키지 않는다. 정상 종료/미전달 취소/worker 사망의 정확1회 해제는 별도 장부 구현으로 검증한다.
이미 FD를 보낸 연결 재분배 금지 유지. Q/W/C/G 숫자·포화 동작·대기 기한·설정 입력·fatal 종료 통지는 이번 선택에 포함하지 않는다.

## 초기 상한 묶음 제안 (미확정)
- W=4 workers, Q=128 미배정 FIFO, C=256 worker별 미종료 연결, G=1152 전체 accepted 미종료 연결(128+4*256).
- shared backend reservation R=1024(4*256), mapping byte budget=1MiB. 예산은 허용 한도이며 1MiB 전체를 mmap/선할당하지 않는다. 실제 크기 계산 후 필요한 길이만 매핑.
- 기능 구현 출발점의 임시값이며 최적값·성능 결과·운영 목표가 아니다.1만 연결 검토 목표는 유지하고 상한 변경·측정은 별도 합의한다. 모든 값·입력/관계 검증은 사용자 선택 후 구현한다.
- 현재 WSL ulimit -Sn=10240, -Hn=1048576 확인. 조회값만 읽었고 한도/OS 설정을 변경하지 않았다. 프로세스별 한도이며 시스템 전체 FD 예산과 같지 않음.
- parent client 원본 FD 최대Q+W=132 제안(추가 listener/IPC/epoll/시그널/관리 FD 제외). 순수proxy worker의 client+backend FD는 최대2C=512 산술 예시. 정적 file/CGI pipe/child·기타 FD와 전체 시스템 자원은 별도 점검.
- 현재 shared layout W4/R1024는232+40*4+32*1024=33160bytes≈32.4KiB(미결정 시간 통계 필드 제외), local validation scratch=96bytes/프로세스. 시간 통계/추가 장부 도입 후 산식 재확인.
- 동시에 배정된1024 연결 모두 header8KiB+body64KiB를 보유한다는 가정은72MiB. relay/응답 buffer·FD/소켓 kernel buffer·프로세스/CGI/parent 장부는 미포함. 서버 전체 메모리 상한 보장 아님.
이번 기록은 제안이며 설정/큐 코드·검증·성능을 실행하지 않았다. 포화 때 accept 보류/즉시 거절은 다음 선택이다.

## 초기 상한 묶음 채택 — 사용자 1번 (2026-11-25)
W=4 고정 프로세스 작업자, Q=128 parent 미배정 전역 FIFO, C=256 worker별 배정 후 미종료 연결, G=1152 전체 accepted 미종료 연결, R=1024 공유 backend 예약 슬롯, 공유 mapping budget=1048576bytes(1MiB)를 초기 임시값으로 채택했다. 최적값/성능 측정 결과가 아니다.
G=Q+W*C, R=W*C. 미배정 FIFO는 worker outstanding에 포함하지 않지만 전체 accepted 수에는 포함한다. SENDING/SENT/worker 처리로 이동해도 같은 연결을 중복 계산하지 않는다. partial sender는 worker assigned 상태의 일부다.
parent 보유 client 원본 FD 최대Q+W=132, 현재 shared layout33160bytes≈32.4KiB, validation scratch96bytes/프로세스라는 앞선 산식은 현재 타입 기준 예산 설명이며 실제 전체 메모리/RSS 상한이 아니다. budget1MiB를 전부 선할당하지 않고 필요한 길이만 mmap. 통계/다른 구조 추가 후 산식 재계산·budget 검사.
기존1만 연결 검토 목표를 대체하지 않으며 더 큰 상한 적용/비교는 별도 합의. worker/backend 네 알고리즘은 최종 미확정. 큐 포화 처리·대기/partial 기한·실행 설정 입력·epoll 모드·fatal 종료/재시작은 이번 수치 승인에 포함하지 않는다.
아직 코드의 실행값을 설정하거나 main에 적용하지 않았다. 포화 동작 선택 후 합의된 parent queue/control 흐름을 구현한다. 부하/성능 실행 승인 아님.

## 포화 동작 확정 — 사용자 1번 (2026-11-25)
Q 또는 G 상한에 도달하면 새 accept를 보류, 여유가 생기면 재개. 기존 client/IPC/종료 처리는 계속. HTTP503/accept후즉시닫기는 추가하지 않는다. kernel backlog의 대기/연결 실패 가능성과 app queue의 구분 유지.
첫 구현 단위: 고정 배열 ring FIFO의 생성/이동/해제와 parent_accept_has_capacity. FIFO는 parent만 소유하고 thread/shared mutex를 사용하지 않는다. accepted FD/ID/accept시각만 저장, parent HTTP 파싱/worker선택은 수행하지 않는다. 상한이 차면 enqueue 자체는 ENOSPC/FD 소유권 유지, 실제 parent loop는 accept 전 gate를 확인해 그 상황을 예방한다.
초기 W4/Q128/C256/G1152/R1024/1MiB는 concurrency_limits.h 한 곳에 기록한다. 현재 main 실행값/CLI에 적용하지는 않는다. parent 전체 연결 장부/worker 종료통보/epoll 등록·해제는 후속 통합이며 gate가 존재한다고 서버가 이미 포화 보류 동작을 수행한다고 주장하지 않는다.

## parent FIFO/gate 단위 완료 (2026-11-25)
concurrency_limits.h의 임시 상한6개, parent_client_queue.h/c의 고정ring FIFO·FD이동/정리·queue/global/stopping gate 구현. 일반 컴파일/ASan·UBSan unit 성공, make test12개 통과. Q128wrap/full·G경계·FD count/cleanup 및 실제loopback 테스트driver의 accept보류/재개·STREAM연결 검사 통과. 상세 iteration-5-parent-queue-review.md.
main/실제epoll·전체연결장부/workerC256/종료통보·fatal전체종료는 미통합. FIFO4128bytes와gate는 전체서버자원/동시성보장이 아니다. 성능/서버E2E 미실행.
