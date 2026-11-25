# backend 예약 수명 구현 리뷰

2026-11-25. 사용자1번: backend 프레이밍 검증 완료 또는 실패·취소에서 해제하고, 남은client출력과 수명을 분리한다. `backend_request_lifetime.h/c`에 연결별 상태와 기존 공유 원장 API 조율을 구현했다. 실제 worker HTTP 상태머신은 아직 미통합이며 제품 서버는 순차 처리다.

## main부터 통합할 함수 순서

main → parent accept/dispatch → worker 헤더·본문·라우팅 검증 → 공유잠금획득·backend정책선택 → `begin_backend_request_locked()` → nonblocking connect/send/receive → 프레이밍 정상 완료 또는 실패/취소 → `mark_backend_request_finished()` → `try_release_backend_request()` → 남은 client 출력·연결 정리 → CLOSED.

`begin_backend_request_locked()`는 선택 때 얻은 같은 잠금을 요구하며 기존 reserve_backend_request_locked를 호출한다. 성공해야 ACTIVE와토큰을게시한다. connect시도전진행수에포함하며 lock 밖 네트워크I/O를 caller가 담당한다. 선택알고리즘·정책최종값은확정하거나구현하지않았다.

`mark_backend_request_finished()`는 ACTIVE→RELEASE_PENDING, 첫 outcome(COMPLETE/FAILED/CANCELLED)을보존한다. 이 함수는프레이밍을파싱하지않는다. caller가고정길이정확완료·종료청크와trailer검증·정상EOFframing·204/304등최종무본문헤더검증후에만COMPLETE로호출해야한다. 1xx/불완전본문/임의EOF는성공이아니다. 실패/취소시backendFD정리도caller가lock밖에서수행한다.

`try_release_backend_request()`는최대1회trylock한다. EBUSY면1을반환하고token/outcome/RELEASE_PENDING을그대로유지한다. 성공하면기존generation-awarecomplete를통해공유기여분을한번감소하고RELEASED로바꾼다. 이미해제된/과거token도현재예약을감소시키지않는다. unlock실패나불신상태오류는토큰을보존하며기존fatal종료경로를caller가진행해야한다.

`reset_backend_request_lifetime()`은IDLE/RELEASED에서만가능하다. ACTIVE/pending을초기화해예약을잃는것은EBUSY로거절한다. worker가사망하면이process-private객체를읽지않고parent가공유원장의worker세대예약을회수한다.

이모듈은FD/heap/시계/프레이밍버퍼를소유하지않는다. worker의trylock재시도간격·깨우기는아직미정이며부모10ms를자동적용하지않는다. releasepending동안공유수는보수적으로남고연결slot을토큰없이재사용해서는안된다.

## 컴파일·실제 검증

| 항목 | 결과 |
| --- | --- |
| strict C11/O2 테스트훅없는 모듈 컴파일 | 통과 |
| make all | 통과 |
| ASan/UBSan make test 전체17개 | 통과(exit0) |
| 같은잠금아래예약·lock없는begin거절·시도전진행수증가 | 통과 |
| 정상/실패/취소 각각첫결과유지·중복해제무변경·reset/begin보호 | 통과 |
| 실제fork잠금경합EBUSY의pending token보존·다른worker기여유지·unlock후해제 | 통과 |
| backend해제후에도미송신바이너리출력·worker연결수유지 | 통과 |
| 실제socketpair SHUT_WR 후에도응답수신가능 | 통과 |
| ACTIVE/pending/released 3경계실제SIGKILL→waitpid→parent세대회수반복 | 통과 |
| 과거token의재사용예약보호·sharedfatal오류시토큰/불신원장보존 | 통과 |
| FD개수복원 | 통과 |

최종로그: build/iteration5-capability/unit-tests-with-backend-lifetime.log.

half-close시험은수명객체와socketpair조합이다. 실제HTTP event handler가EOF/RST를구별하는통합E2E가통과했다는뜻은아니다. 고정길이/chunked/trailer/EOF/1xx/무본문프레이밍기존회귀는make test에포함하지만새완료hook의실제연결은아직없다.

## 비용·남은 작업

현재WSL ABI에서객체32바이트. C256×W4에서최대32,768바이트(32KiB)의연결별상태예산이며원장·HTTP·버퍼·kernel소켓메모리는별도다. 실제동시할당·서버메모리/성능실측이아니다. 정상경로는예약잠금과완료trylock, 생성후추가할당없음.

제품main/workerHTTP·proxy nonblocking전환, releasepending재시도·출력버퍼/역압력, 시간통계/점수/설정입력, log sink·부모사망·CGI/파일I/O는남아있다. 제품make verify E2E·부하·성능·커밋/머지/푸시를실행하지않았다.
