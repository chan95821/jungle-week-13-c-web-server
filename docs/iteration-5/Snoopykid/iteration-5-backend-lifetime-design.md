# backend 진행 수 수명 — 정책 연결 전 선택

2026-11-25. 기존 기록에서 ‘선택·예약부터 backend 응답 프레이밍 완료 또는 실패/취소까지’는 제안으로 남아 있으며 최종 채택 기록이 없다. worker 진행 수(배정 후 미종료 클라이언트 연결)와 별개로 다음을 선택한다. 네 알고리즘 후보와 최종 미확정 상태는 유지한다.

## 1 추천: backend 중계 작업 수명

- 요청 구문·프록시 경로·본문 수신을 완료한 뒤, backend 선택과 예약 증가를 같은 공유 잠금 구간에서 수행한다. 연결 시도 전에 증가하므로 connect 대기도 포함한다. 네트워크 I/O는 잠금 밖이다.
- Content-Length의 정확한 바이트 완료, chunked의 종료 청크와 trailer 검증 완료, EOF framing의 정상 EOF, 204/304 등 본문 없는 최종 응답 헤더 검증 완료를 backend 정상 완료로 판단한다. 1xx만 받고 끝난 상태는 정상 완료가 아니다.
- 마지막 backend 바이트가 제한된 중계 버퍼에 보존되고 framing 검증을 마쳤으면 해제를 예약한다. 클라이언트에 아직 남은 출력이 있다는 이유만으로 backend outstanding을 계속 유지하지 않는다. 하지만 역압력 때문에 backend에서 마지막 바이트를 아직 읽지 못했다면 계속 포함한다.
- 연결 거절·시간 초과·잘못된 응답·조기 EOF/청크 손상·클라이언트 취소는 backend FD를 정리하고 예약 해제를 요청한다. worker는 취소 후 backend 응답을 계속 버리며 기다리거나 자동 재시도하지 않는다.
- 클라이언트의 recv EOF만으로 취소라고 단정하지 않는다. 요청을 모두 보낸 뒤 shutdown(SHUT_WR)한 클라이언트는 응답을 받을 수 있다. 불완전 요청 EOF는 기존400, 응답 송신 실패/RST 등으로 응답 전달이 불가능해졌을 때 취소를 처리한다.
- 완료 lock EBUSY면 토큰을 가진 RELEASE_PENDING을 유지한다. 실제 공유 감소 전까지 outstanding에 보수적으로 남고, 중복 완료/새 backend 선택으로 재사용하지 않는다. worker 종료 시 부모가 기존 세대 원장으로 회수한다. worker측 lock 재시도/요청 slot 재사용 기한은 별도 이벤트 루프 선택이며 부모10ms 정책을 자동 적용하지 않는다.

장점: Least Outstanding가 backend 관련 대기를 나타내고 느린 클라이언트의 잔여 출력 대기와 구분된다. 비용: backend 완료·클라이언트 완료 상태를 분리하고 출력 버퍼·토큰의 수명을 따로 관리해야 한다.

## 2 대안: 클라이언트 연결 종료까지 유지

선택/예약 시작은 같지만 클라이언트 응답·마무리 종료까지 backend 카운트를 유지한다. 연결 단위 정리와 함께 처리하기 쉽지만 backend가 이미 끝난 후에도 느린 클라이언트/마무리 대기가 backend 부하로 반영된다. worker outstanding과 겹치는 의미가 커진다. backend 처리 중인 실제 원 서버 요청 개수를 정확히 알 수 있다는 뜻은 어느 안에서도 아니다.

## main부터 목표 흐름

main → parent accept/dispatch → worker 요청 수신·파싱·본문 완료 → 공유 잠금 아래 정책 선택+reserve_backend_request_locked → nonblocking connect/send/receive → framing 완료 또는 실패/취소 → complete_backend_request_locked → 잔여 client 출력·연결 정리 → CLOSED → parent 장부 해제.

위 흐름은 통합 목표이며 현 제품 main은 순차 처리다. 기존 원장 API가 token의 정확1회 감소를 제공하지만 HTTP 완료 시점 연결은 아직 구현하지 않았다. 응답 시간 통계의 구간·평균·초기값·점수·실패 반영·정책 설정은 이 선택으로 확정하지 않는다.

## 검증 후보

connect 대기 포함, 고정길이/청크+trailer/EOF/204/304/1xx 경계, framing 완료 후 client 출력 대기 중 backend 해제·worker 유지, 역압력으로 읽기 미완료일 때 유지, 클라이언트 정상 half-close와 RST 구분, 부분 최종 응답 뒤 새 오류 미첨부, 완료lock EBUSY의 pending 토큰 보존·사망회수·중복감소방지. 실제 HTTP 상태머신 구현 전 테스트 후보로 먼저 누적한다. 부하·성능 실행은 포함하지 않는다.

## backend 진행 수 수명 1번 확정 (2026-11-25)
요청본문완료 후 공유잠금 아래 선택+예약→connect/송신/응답 수신→backend 프레이밍 검증 완료 또는 실패/취소에서 해제 요청한다. 남은client출력/마무리와 분리. 역압력으로backend완료미확인시 ACTIVE유지. half-close자체는취소가아님. 완료lock EBUSY는RELEASE_PENDING토큰보존, 공유감소확인전상태재사용금지. worker측재시도간격은아직미결정이며부모10ms자동적용없음.
첫구현단위는worker연결이소유하는backend_request_lifetime: begin_locked(기존잠금/선택결과로예약), mark_finished(완료/실패/취소), try_release(1회trylock과해제), reset(해제확인후만). FD/본문/프레이밍parser를소유하지않으며실제HTTP상태머신연결은다음단위다. 성공판정은parser가프레이밍검증후호출하며임의EOF를성공으로바꾸지않는다.

## backend lifetime 단위 완료 (2026-11-25)
backend_request_lifetime.h/c에begin_locked→ACTIVE→mark_finished→RELEASE_PENDING→try_release→RELEASED/reset구현. 공유선택과예약동일잠금·EBUSY토큰보존·중복첫outcome유지·재사용보호. 실제fork경합/3경계SIGKILL·parent회수·잔여client출력/worker수분리·half-close socketpair시험포함ASan/UBSan전체17개통과, 일반O2/make all성공. 객체32bytes(현재ABI). 실제HTTP프레이밍완료hook/worker상태머신·release재시도미통합/미결정;기존parser회귀와구분. 성능/E2E/커밋/머지/푸시없음. 상세 iteration-5-backend-lifetime-review.md.
