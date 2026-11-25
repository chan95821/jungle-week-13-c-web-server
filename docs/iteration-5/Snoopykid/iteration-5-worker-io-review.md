# Iteration 5 worker nonblocking HTTP I/O 구현 리뷰

2026-11-25. 사용자가 추천 묶음의 구현을 승인했다. 실제 fork 작업자가 기존 FD 채널을 받아 여러 TCP client/backend를 LT epoll로 처리하는 네트워크 reactor를 구현했다. 제품 ./build/server main은 아직 순차 처리다. 정적 파일·CGI 전환과 부모 실행 통합을 끝내기 전에 제품 진입점을 바꾸지 않았다.

## 실행 가능한 구성요소와 확인 명령

```
make all worker-io
make test_worker_http_io test_worker_io_state
./build/test_worker_http_io
./build/test_worker_io_state
```

worker-io는 일반 C11/Wall/Wextra/Werror/O2 객체를 빌드한다. 두 실행 파일은 ASan/UBSan 기능 검증이다. 첫 시험은 실제 fork/SCM_RIGHTS/TCP/C backend, 두 번째는 실제 socket I/O와 주입한 시각의 기한 경계를 다룬다. 테스트용 C backend만 large-body(4MiB) 모드를 추가했다. Python 서버·부하 생성기·외부 HTTP 라이브러리는 사용하지 않았다.

## main부터 실제 함수 순서

현재 테스트의 main은 C backend3개와 단일 parent listener를 준비하고 공유 세대를 활성화한 뒤 worker를 fork한다. parent는 실제 TCP accept와 기존 prepare_worker_fd_transfer/advance_worker_fd_send를 사용한다. 제품 부모 supervisor/dispatch 루프를 통합했다는 뜻은 아니다.

1. child의 prepare_process_signal_wait → create_worker_http_reactor: borrowed config/shared view/worker IPC FD를 받고 고정 슬롯·CLOSED 큐·epoll을 만든다. 상속한 타 FD 정리는 caller 책임이다.
2. advance_worker_http_reactor: 종료/fatal 우선 확인 → worker_http_service_deadlines → 최소 기한 timeout의 epoll_pwait → 종료/fatal 재확인 → 세대 event token 검증 → client/backend/channel별 진행 → 다시 기한·관심 이벤트 갱신.
3. advance_channel → advance_worker_fd_receive → admit_received_client: 프레임 전체 검증 후 FD와 메타데이터를 안정된 슬롯에 옮긴다. worker 인계 시각과 parent accept 시각을 구분한다. active+미송신 CLOSED가 C를 넘으면 새 FD 수신 관심을 보류하고 기존 I/O·CLOSED 송신은 계속한다.
4. advance_client_event → read_client_request → parse_client_headers: 부분 수신, 헤더 끝,8KiB상한, 기존 http_parse_request/http_select_route/proxy_validate_request를 재사용한다. 경로·메서드·Host·길이 오류는 malloc/100/backend 전에 거절한다. 헤더 동반 본문을 보존하고 선언 길이만큼 받으며 본문은 byte 배열이다.
5. Expect가 있고 본문이 더 필요하면 WORKER_INTERIM_WRITE에서100을 부분 송신한다. 정상 완료 뒤 CLIENT_BODY로 이동한다. 요청 body 전체 완료 후 request_ready가 LOCAL_READY 또는 BACKEND_SELECT로 이동한다.
6. worker_proxy_timer → select_backend: pending일 때만10ms 간격으로 trylock한다. 최초 선택 시도부터1초가 지나면 새503을 준비한다. 공유 선택·예약은 같은 transaction, 네트워크 I/O는 lock 밖이다. 제출은 고정 backend이며 조회의 cursor/탐색 횟수를 소비하지 않는다.
7. begin_backend_connection: socket NONBLOCK/CLOEXEC → connect → EINPROGRESS 상태 보존. worker_proxy_advance가 writable 뒤 SO_ERROR를 확인하고 WORKER_BACKEND_REQUEST로 옮긴다. poll로 기다리지 않는다.
8. write_backend_request: 기존 순차 코드에서 분리한 순수 build_backend_request_headers를 재사용한다. Host 재작성/hop-by-hop/길이/Via 규칙이 같다. 헤더·바이너리 body의 남은 부분만 send하며 이벤트 예산에서 yield한다.
9. read_backend_headers → finish_backend_headers: 기존 parse_backend_headers/build_client_response_headers를 재사용한다. 1xx를 제한된 출력 공간에 보존하고 최종 응답을 검증한 뒤 client 송신을 허용한다. backend HTTP1.0/1.1·상태문구·204/304 규칙은 유지한다.
10. read_backend_body: 고정 길이·청크 확장/크기·trailer·EOF를 상태로 검증한다. 청크와 EOF는 기존처럼 client chunked로 재프레이밍하며 정상 완료 전 종료0청크를 만들지 않는다. trailer 금지/Connection 지명 규칙도 재사용한다.
11. worker_http_backend_finished: framing 완료 순간 시각을 고정 → backend FD 정리 → 예약 해제 시도. 해제 EBUSY면 token을 안정된 슬롯에 유지하고10ms 기한에만 재시도한다. client 종료 경로도 기한 전 trylock을 반복하지 않는다.
12. write_client_output: 헤더·메모리/relay의 남은 바이트만 보낸다. 첫 최종 송신 시도 뒤 실패에는 새502/504를 붙이지 않는다. relay가 차면 backend 읽기 관심을 빼고8KiB 이하에서 worker_http_resume_backend_input으로 재개한다.
13. client 출력 완료 → shutdown(SHUT_WR) → CLIENT_DRAIN에서 최대1초/64KiB만 읽어 버린다. 정상 half-close 자체는 취소가 아니다. backend 해제·client FD/버퍼 정리가 끝나면 worker_http_publish_completed가 CLOSED v2를 enqueue한다. sender가 기존 채널 역방향으로 부분 송신한다.
14. destroy_worker_http_reactor는 worker exit 경계에서 owned FD/버퍼/epoll을 정리한다. IPC FD/shared mapping은 borrowed다. 종료 중 남은 공유 예약을 임의 해제했다고 표시하지 않고 부모가 실제 waitpid 확인 후 세대별 회수한다. 같은 worker 세대의 처리를 destroy 뒤 재개하면 안 된다.

직접 작성한 함수들이 위 상태를 조율한다. socket/connect/getsockopt/recv/send/shutdown/epoll 및 시계·시그널은 OS API다. send_all_with_deadline/receive_with_deadline/기존 blocking handle_client를 reactor 안에서 호출하지 않는다.

## 로컬 처리 경계

worker_http_local_request가 LOCAL_READY인 요청을 노출한다. resume_worker_http_memory_response는 준비된 안정된 메모리 응답을 받아 nonblocking 송신을 재개한다. body는 caller가 CLOSED까지 보존하며 reactor가 임의 free하지 않는다.

이번 fork fixture는 /hello와 /echo의 메모리 응답만 이 경계로 연결했다. 제품의 파일·greet 템플릿·CGI handler를 blocking callback으로 호출하거나 기능을 제거하지 않았다. 해당 I/O 방식 선택과 실제 main 통합은 남아 있다. 따라서 ‘모든 제품 경로가 동시 처리된다’고 표현하지 않는다.

## 버퍼·기한·공정성

- 요청 줄/헤더8KiB는 고정 슬롯 안, 본문은 검증된 Content-Length만큼 최대64KiB 단일 malloc이다.
- relay는 필요 단계에서 할당하는 proxy_stream.buffer의16KiB를 재사용한다. 별도의 응답 전체 할당은 없다. 선형 offset과 bounded memmove를 사용한다. 청크 재프레이밍은 단일 worker stack의 임시16KiB 입력 공간을 이용한다.
- high16KiB/low8KiB다. 다음 최소 청크 프레임이 남은 공간에 들어가지 않으면 마지막 몇 바이트를 넘치게 쓰지 않고 조기에 보류한다. trailer 전체는 검증 후 종료청크와 함께 넣으며, 공간이 부족하면 검증된 trailer를 보존하고 backend 예약부터 해제한다. 잔여 client 출력이 backend 통계를 늘리지 않는다.
- client 수신·송신 무진행5초는 실제 양수 byte 진행으로만 갱신한다. EAGAIN/ready/EINTR로 늘리지 않는다. 읽기를 의도적으로 보류할 때 backend idle의 남은 예산만 저장하고, 재개 시 남은 예산을 사용한다. backend전체15초는 계속 흐른다.
- header7초는 parent accept부터, body15초는 header완료부터다. connect1/request송신5/header1/backendbodyidle5/전체15와 drain1초/64KiB를 유지한다. 부분100 송신 도중 body 기한이 끝나면 미완성 중간 응답에408을 이어 붙이지 않고 연결을 닫는다.
- FD 이벤트는64KiB 또는16회data/connection확인 syscall 예산에서 yield하며 batch64개다. IPC는 recvmsg 외 FD 소켓 검증도 보수적으로 예산에 포함한다. resource close/epoll 관리와 parser 계산의 CPU 시간을 이 숫자로 보장하지 않는다.
- C개의 안정된 슬롯을 매 turn 스캔하며 epoch/slot/backend 구분 token으로 stale epoll event를 무시한다. 이 구조가 C10K의 최적 구현이라고 판정하지 않는다.
- 현재 backend 헤더·chunk metadata는 정확한 경계를 위해 byte 단위 recv로 진행한다.16회 예산에서 자주 yield할 수 있어 syscall 비용이 크다. bulk 수신/peek/별도 입력 버퍼 대안은 추후 성능 조건 합의 후 검토하며 빠르다고 주장하지 않는다.

## 실제 검증 결과

| 구분 | 결과 |
| --- | --- |
| make all / make worker-io, C11 strict O2 | 통과 |
| ASan/UBSan make test 전체20개 실행 파일 | 통과(exit0) |
| 실제 fork worker·FD채널·단일 TCP 접속 포트에서 여러 연결 처리 | 통과 |
| 64KiB binary POST 분할·100 Continue·Host/Via/길이/목적지 유지 | 통과 |
| 느린 header·query A가 남아 있는 동안 query B·로컬·제출 완료 | 통과 |
| 네 query 정책의 실제 TCP 적용·공유 표본/선택횟수 | 통과 |
| chunked/trailer·EOF·1xx·204/304·HTTP1.0/status문구 | 통과 |
| 잘못된 backend·중간 중단·실제 bodyidle timeout, 최종응답 두번째 status 없음 | 통과 |
| 조기400/413/100미송신·8191/8192/8193 header·accept시각 전체기한 | 통과 |
| 실제 mutex 선택 대기503·해제pending token/10ms재시도·회복 | 통과 |
| 실제 작은 socket buffer/느린 reader의 relay보류·다른연결진전·4MiB binary무결성 | 통과 |
| 실제 RST 전/후 worker생존·다음FD frame·FD baseline | 통과 |
| 주입시각 idle예산정지/재개·전체backend기한·clientbody전체기한·clientwriteidle | 통과 |
| 실제 recv/send 예산·stale-generation epoll event 무시 | 통과 |
| 실제 backend대기중SIGTERM·worker FD정리·waitpid 뒤 parent예약회수 | 통과 |
| worker 정상 exit의 sanitizer 검사와 parent/worker FD baseline 복원 | 통과 |

전체 로그: build/iteration5-capability/unit-tests-with-worker-io.log. 이후 네 정책/빠른query B/종료중회수를 보강한 실제 TCP 단위는 worker-http-io-test.log, 기한·부분100은 worker-io-state-test.log, 강제로FD수신전RST를만든단위는 fd-reset-before-delivery-test.log에서 재확인했다. 공유 state 시험의 주입시각을 실제 지연/성능 수치로 보고하지 않는다.

실제 fork 테스트는 C backend와 worker reactor를 실행한다. build/server main을 실행하는 make verify 제품 E2E는 이번에 실행하지 않았다. 정적 파일/CGI의 reactor 통합·실제 W4 parent 배정 전체 E2E·프로토콜 퍼징·커널 자원 고갈은 미검증이다. skip으로 통과시킨 항목은 없다. 부하·처리량·p95·CPU/메모리 성능 측정은 하지 않았다.

## 실패에서 보완한 경계

- 실제 TCP reset이FD전달중발생하면getpeername ENOTCONN이될수있었다. 전송frame전체·대상세대·STREAM유형은정상이고SO_ERROR에ECONNRESET/ECONNABORTED증거가있을때만 WORKER_FD_CLIENT_GONE으로분리해FD를닫고정상CLOSED취소회계를진행한다. 미연결socket(error0)/잘못된FD/frame은여전히제어오류다. 강제로reset을수신전에발생시킨뒤다음frame이정상인지검증했다.
- 제출예약이조회탐색counter를0으로되돌리던기존호환wrapper를수정해현재count를보존했다. 제출은조회순번/탐색주기에영향을주지않는회귀를추가했다.
- 초기 시험의 trailer문구는기존 C fixture의‘complete’로맞추고NUL이후는strstr대신길이기반으로검사했다. 최종송신전발견한잘못된backend는502가가능하고, 송신후는두번째응답없이종료하는규약으로시험을구분했다.413문구도기존Content Too Large를유지했다.
- compiler의 들여쓰기·서로다른enum 비교 경고를 정리했다. 기존 요청 헤더 생성은 순수 함수로 분리하되 순차5초 기한의 시작점은 유지했다. 실패 시험을 제거하거나RPS를측정해성공으로대체하지않았다.

## 저장 공간과 남은 작업

현재 WSL ABI: 연결slot9256, reactor216, proxy_stream16592(relay포함), response헤더workspace24880, IPCreceiver64바이트다. C256/W4의고정slot+reactor+완료token배열+수신상태는약9.09MiB다. body64KiB·connection지명필드8KiB·trailer8KiB까지모든proxy연결에동시에존재한다고보수적으로계산하면관련heap합계약129.6MiB다. 실제메모리사용량/전체서버예산측정이아니다. shared/parent표·프로세스·epoll/kernel socket buffer·localfile/CGI·worker별임시stack16KiB등은별도다.

제품 부모 loop/고정fork/main통합, localfile/CGI 비블로킹준비방식, logsink/부모사망과CGI수명은남아있다. actual worker 오류/fatal 반환을 parent 긴급통지에 연결하는 최종 조율도 필요하다. 최종분배알고리즘/성능우위/C10K처리가능성은미정이다. 커밋·머지·푸시하지 않았다.
