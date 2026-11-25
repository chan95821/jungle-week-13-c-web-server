# HTTP worker 이벤트 루프 — 버퍼·역압력·기한 선택 묶음

2026-11-25. 기존 결정과 새 제안을 구분한다. 아직 사용자 선택 전이며 이번 문서 작성으로 코드/테스트/계측을 변경하지 않는다.

## 기존 확정 유지

고정 프로세스 worker별 nonblocking LT epoll, parent accept/FD전달, W4/Q128/C256/G1152, 연결당요청1개/close, HTTP·Host·오류·프레이밍 지원, 헤더8KiB/요청본문64KiB/정확길이malloc, 공유선택/복구·네정책·시간구간·종료2000ms는 그대로다. 전체헤더7초는accept시각부터, 본문15초는헤더수신완료부터다. 기존backend connect1/request송신5/header1/bodyidle5/body전체15초, drain1초/64KiB도 바꾸지 않는다.

블로킹 handle_client/receive_with_deadline/send_all_with_deadline을 epoll callback에 넣지 않는다. nonblocking connect의 EINPROGRESS는 상태로 보관하고 writable 뒤 SO_ERROR로 성공/실패를 확인한다. 현재 제품 main을 여러 작업자가 실제 서비스하는 것으로 바꾸려면 아래 네트워크 상태 머신과 local file/CGI 작업까지 연결해야 한다.

## 추천 묶음 1 — 제한 버퍼와 명시적 대기 상태

| 항목 | 제안 | 비용·차이 |
| --- | --- | --- |
| relay 저장 | 프록시 응답용 고정16KiB 선형 버퍼를 필요 단계에서 한 번 할당. read/write offset과 남은 길이로 binary-safe 처리 | 전체 응답 크기로 확장하지 않음. 필요할 때 bounded memmove 가능. ring 대안은 복사 감소 여지가 있지만 경계/청크 구현·검증 증가 |
| 역압력 | relay가16KiB에 차면 backend EPOLLIN 해제. client출력으로8KiB 이하가 되면 재등록. client EPOLLOUT은 실제 보낼 데이터가 있을 때만 등록 | 16/8KiB는 임시 high/low. header/청크크기/trailer의 bounded parser 상태와 client출력헤더 공간은 relay와 별도. kernel socket buffer의 상한을 이 값으로 보장하지 않음 |
| 요청 본문 | 기존 Content-Length만큼 malloc한 최대64KiB를 모두 받은 후 backend선택/전송. backend송신중 부족한 부분만 이어 보냄 | request streaming은 추가하지 않음. 헤더동반본문/남은데이터·100Continue·조기400/413/431 우선순위 유지 |
| 공유lock 대기 | worker측 선택/해제 EBUSY도 pending일 때만 임시10ms 재시도. 각 연결 기한 전에는 반복 trylock하지 않음 | parent10ms를 묵시적으로 물려받지 않고 이번 선택으로 별도 채택하는 안. 네트워크/다른연결/종료 계속 |
| 선택lock 획득 상한 | 요청 준비 완료 후 최초 선택 시도부터 임시1초. 예약을 얻지 못하면503으로응답·종료, backend연결/재시도없음 | 새503상황이다. 1초는 backend connect timeout과 별도. 응답을이미시작했다면새오류를붙이지않음 |
| 해제lock 대기 | backend/client FD와버퍼 정리는가능한대로하되 RELEASE_PENDING 토큰을안정된연결slot에보존.10ms로재시도하고공유해제전slot재사용/새배정여유로계산하지않음 | 선택대기처럼예약을버리거나임의감소하지않음. 정상운영중해제자체의새강제deadline은두지않음. 잠금소유자가살아멈추면용량이묶일수있으며운영정지시기존worker종료/사망복구로회수. 별도watchdog자동추가없음 |
| 호출5초의 전환 | EAGAIN을기다리는각방향에실제바이트진행기준무진행기한을둠. client수신/송신5초, backend본문5초. 단순ready/EINTR/EAGAIN으로갱신하지않음 | nonblocking syscall호출자체에5초를걸지않음. 현재블로킹의호출대기를이벤트대기로표현하는안이며동작변환을명시 |
| 역압력과 기한 | backend읽기를의도적으로보류하는동안본문idle잔여시간만정지, 재개하면남은idle예산을사용. backend전체15초는계속흐르고client송신idle5초도유지 | 의도적읽기보류를backend무응답으로중복판정하지않음. 느린client로전체deadline이끝날수있음. 전체기한/idle을매ready마다리셋하지않음 |
| 이벤트 공정성 | FD 이벤트당 최대64KiB 또는16회네트워크syscall 중먼저도달하면yield. EAGAIN이면즉시yield. epoll반환batch64개 | LT이므로다음반복에서진전. 숫자는임시,CPU상한/공정성보장실측아님. parser는기존고정상한내에서처리 |
| 기한 관리 | worker는최대C256개의안정된slot을매반복스캔해가장가까운기한을epoll timeout에반영 | heap구현없이단순,연결수증가에따른O(C)비용. C10K최종구조/최적값이라고하지않음 |
| epoll 수명 | 연결slot+재사용세대와client/backend구분을event token으로사용, FD정수만으로현재연결판정하지않음 | 같은batch에남은stale event가재사용FD/새연결을건드리지않게검증. free된pointer를event data로사용하지않음 |

최종응답의첫송신시도이후오류면새502/504를붙이지않고불완전응답으로종료한다. client half-close를RST와같이취급하지않는다. EOF본문은진짜EOF, chunked는종료청크/trailer검증까지확인하며기존1xx/204/304/상태문구/Via를유지한다.

## 대안 2

응답relay를64KiB ring으로하고high64/low32KiB를사용할수있다. 더많은데이터를미리읽어backend를일찍해제할여지가있지만연결별메모리와원형경계검증이늘어난다. 더빠르다는판정은없다. 공유lock선택상한을새로두지않고기다리는안은503상황을줄이지만선택대기의상한이없어진다. 이대안들을원하면묶음중해당행만바꿔선택할수있다.

## main부터 실제 구현 순서

main시작/고정fork → parent LT(accept/FIFO/FD/CLOSED/감독) → worker FD프레임수신 → CLIENT_HEADER → CLIENT_BODY/INTERIM_WRITE → 로컬 또는 BACKEND_SELECT_PENDING → BACKEND_CONNECT → BACKEND_REQUEST_WRITE → BACKEND_HEADER → BACKEND_BODY/CLIENT_WRITE(역압력) → BACKEND_RELEASE_PENDING → CLIENT_DRAIN/정리 → CLOSED.

이는상태별목표이며상태를직렬로만진행시키지는않는다. backend읽기와client쓰기는같은연결에서동시에관심등록될수있고, backend해제후잔여출력·client종료를기다리는동안다른연결을처리한다. local file/CGI의블로킹동작은별도설계선택후연결하며, 그선택없이기존기능을꺼버리거나블로킹callback으로우회하지않는다.

## 메모리 추정

W4/C256에서relay만최대16MiB,요청헤더8MiB,요청본문모두64KiB를할당한다면64MiB다. 이것만합쳐88MiB이며응답헤더/백엔드헤더/trailer/parser상태·원장·프로세스·FD/kernel socket buffer·파일/CGI는별도다. 단계별수명이겹치는공간은구현후sizeof와함께다시합산한다. 메모리실측/성능보장이아니다.

## 선택 후 검증 후보

- 실제TCP에서한연결이header/body/backend/client송신을멈춰도다른연결과제출요청진전.
- relay16/8KiB경계,관심해제/재등록뒤데이터유실·busyloop없음,부분read/write와binary/framing무결성.
- header/body동반/분할·기존8/64KiB경계/조기오류와100Continue·정상halfclose/RST.
- shared선택대기503·해제pending보존·10ms기한전trylock없음·worker사망세대회수.
- idle일시정지/재개잔여시간·전체기한계속·실제바이트진행만idle갱신.
- 이벤트예산yield·stale event/FD재사용·다중종료·모든정상/오류경로FD/버퍼정리.
- 기존단위회귀와제품E2E범위를구현단위완료후구분제시. 부하·처리량·지연비교는별도허락.

## worker I/O 추천 묶음 구현 승인 (2026-11-25)
사용자‘구현’으로16KiB/8KiB역압력·worker pending10ms·선택lock1초후503·해제token보존·진행기준idle/읽기보류중backendidle잔여정지/전체기한유지·64KiB/16syscall·epollbatch64·slot기한스캔/세대eventtoken을채택했다. 첫실행단위는기존FD STREAM을받는실제worker epoll reactor와nonblockingclient/backend상태다. localfile/CGI는미정이므로LOCAL_READY에서외부비블로킹응답준비로넘길경계를노출하고기존main은유지한다. 정적/CGI를꺼버리거나blockinghandler로우회하지않는다.

## 실제 worker nonblocking 네트워크 단위 완료 (2026-11-25)
worker_http_io.h/c·worker_http_internal.h·worker_proxy_io.c에기존FD/CLOSED채널+실제LT epoll다중연결·요청/100·backend선택/connect/송신/헤더/본문·16/8KiB역압력·worker10ms/선택1초503·해제pending·전체/idle기한·예산·세대eventtoken구현. 기존pureheader/parser재사용. FD전달중증거있는TCPreset은CLIENT_GONE으로연결취소처리,다음frame/FD회수검증. strictmake all/worker-io및ASan/UBSan전체20개통과,실제fork/TCP/Cbackend/4MiB binary/느린A중B·제출진전/네정책/SIGTERM대기중회수확인. 상세 iteration-5-worker-io-review.md. 제품main은여전히순차,localfile/CGI/부모실행통합/긴급통지최종조율미완료. make verify제품E2E·부하/성능·커밋/머지/푸시없음.
