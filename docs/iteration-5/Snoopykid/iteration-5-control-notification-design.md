# LT와 worker 제어·긴급 통지 — 구현 전 선택안

2026-11-25. 사용자 확정: epoll Level-Triggered(LT). EPOLLET/ONESHOT 사용을 자동 추가하지 않는다. FD/client/backend 모두 nonblocking, 이벤트당 공정한 처리량·역압력 상한은 별도 미정.

## 이미 확정된 경계 유지
client FD 전달은 worker별 SOCK_STREAM/SCM_RIGHTS/noACK 그대로 유지한다. parent는 accept/분배/worker관리, worker는 HTTP/backend. 긴급 통지 경로 선택은 client FD 채널을 SEQPACKET/다른기술로 바꾸는 선택이 아니다.
worker CLOSED 일반 통지는 기존 full-duplex STREAM의 worker→parent 방향으로 보낼 수 있다. parent→worker FD전송과 반대 방향의 buffer는 구분된다. 다만 CLOSED와 FATAL이 같은 worker→parent 전송 경로에 쌓이면 FATAL 상세 메시지는 일반 메시지 뒤에 놓일 수 있다.
현재 shared backend fatal latch와 local bounded diagnostic은 구현됐으나 부모 깨우기·진단 보존·실제 전체 종료는 아직 미통합이다.

## 선택1 추천: 기존 STREAM + 긴급 nonblocking pipe + 고정 진단 슬롯
- 일반 CLOSED는 STREAM 제어 frame. 긴급 보고는 worker가 자기 고정 진단칸을 완성/게시하고 shared fatal 표시 후 pipe에1byte 알림.
- pipe는 전체 worker가 공유하는1쌍, parent가 read end/epoll 감시, 각 worker가 write end만 보유. fork 후 불필요한 사본 close. pipe 내용은 깨우기 신호이며 요청/진단문자열/FD를 전달하지 않는다.
- parent는 깨어나면 shared fatal 확인 → accept중단 → 준비된 bounded diagnostics 확인/출력 → 종료 절차. 로그가 막혀도 종료가 멈추지 않는 sink/기한 정책은 별도 선택/구현.
- pipe full의 EAGAIN이면 이미읽을 알림이 있으므로 다시 쌓으려고 block/spin하지 않는 안. pipe flag확인/배출 전후 순서와 EINTR/EPIPE·parent죽음 fallback은 구현 전 구체화. 단순 pipe추가만으로 모든 lost wake/시그널 종료 문제가 해결된다고 표현하지 않음.
- 추가 비용: global pipe1쌍(생성FD2, 각process불필요사본정리), 긴급발생 때 write, worker별 고정 숫자 진단 슬롯. 정상 요청마다 긴급pipe syscall은 하지 않는 안. 실제 CPU/성능은 미측정.
- 진단 슬롯은 worker 하나당1개, 세대/영향받은worker·검사enum/phase/stage/종료이유 숫자만. 대략32~64bytes/worker 가정(W4이면128~256bytes)이며 ABI/필드/atomic게시 방식 확정 후 정확 비용 재계산. payload/가변문자열/무제한dump 금지.
- 자기 슬롯 단일 writer, 완성 게시 전 parent가 payload를 읽지 않는 원칙 제안. 게시 도중 worker사망이면 incomplete/unknown 기록과 실제사망 정보만 남김, 알 수 없는 원인을 꾸며내지 않음. 임의 공유메모리 손상까지 믿을 수 있는 audit log가 되는 것은 아님.

## 선택2: 기존 양방향 STREAM만 재사용
- CLOSED/FATAL을 고정 frame으로 전달. 추가pipe는 없고 worker별큐/partial송신 관리가 필요.
- shared fatal 확인으로 종료판단과 상세frame을 분리하더라도 전달대기/통지실패/worker사망 중 parent가 epoll에 잠들어 남지 않도록 별도 wake 또는 최대wait/fallback 정책 필요. 주기적확인을 쓰면 idle wake비용·확인지연이 추가되며 간격은 별도 선택.
- 상세진단은 메시지 전송 전 사망·적체로 유실될 수 있다. 고정공유진단칸을 함께 둘지/유실표시 수준도 선택 필요. 더 빠르거나 더 안전하다고 미리 단정하지 않는다.

## 추가 미결정과 검증 후보
채널/slot 선택 후 CLOSED frame 형식과parent장부·worker세대 통보, 긴급게시순서·firstfailure유지·worker사망, managedsignal과epoll 대기경합 방지·부모사망·종료기한·로그sink 정책을 설명해 결정/구현한다.
검증: 일반 STREAM송신 buffer full에도긴급경로가 parent를깨움, pipe포화/동시보고/EINTR/EPIPE, 게시전/중/후 worker사망, 준비안된payload 읽지않음, oldgeneration슬롯 오염없음, accept중단과원본/queued/workerFD 회수. 기능장애시험과성능측정은구분.
이문서는 선택안이며 새code/test/측정을 실행하지 않았다.

## API 근거
https://man7.org/linux/man-pages/man7/pipe.7.html
https://man7.org/linux/man-pages/man7/epoll.7.html

## 긴급 경로 채택 — 사용자 1번 (2026-11-25)
일반 CLOSED는 기존 STREAM worker→parent, 긴급 알림은 전역 nonblocking pipe1쌍+worker별 고정 진단칸을 채택. client FD STREAM/noACK는 유지. 고정 진단은 reporter와대상worker/세대·검사/phase/stage·종료이유 숫자만, 가변문자열/body/headers/URI 없음.
첫 단위는 공유slot 초기화/arming·원자게시·읽기·boundedformat과pipe 생성/알림/소비. slot은 worker당1개(상한W), firstreport를 같은세대에서 덮어쓰지 않음. parent는 초기fork전 또는 waitpid로종료확인후만 다음세대를arm, 자동재시작은하지않음. slot publication EMPTY→WRITING→READY, ready전payload를읽지않음. 게시도중사망은 incomplete/unknown으로분류하고원인을추정하지않음.
parent의실제SIGCHLD/epoll 대기경합·로그sink적체/강제종료기한·전체종료는후속통합. 특히 게시후pipe쓰기전사망은pipe알림이없을수있어SIGCHLD 경로가필요함. 이단위가그전체wake/종료보장을완성했다고주장하지않는다.

## 긴급pipe·고정진단 단위 완료 (2026-11-25)
worker_failure_channel.h/c와version3 sharedslot 추가. fatal의도→WRITING→READY 게시, firstreport유지·generation차단·READY전payload미조회, 별도nonblocking pipe알림/소비와제한숫자/unknownformat 구현. slot64bytes·W4추가256, 초기mapping33416bytes/budget1MiB유지.
일반컴파일성공, make testASan/UBSan13개통과. 실제LT/pipe·STREAM포화/EPIPE·5경계SIGKILL/다중게시/손상/복구불가mutex·FD회수검증. 상세 iteration-5-failure-channel-review.md. main/SIGCHLD/실제전체종료·로그sink적체/기한은미통합이며특히알림전사망은SIGCHLD경로가필요하다. 서버E2E/성능미실행.
