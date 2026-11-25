# 네 분배 정책 — 구현 전 세부 규칙 묶음

2026-11-25. 시간 구간은 1번으로 확정했다. 아래는 아직 미채택 제안이며 코드·프레임·공유 layout을 변경하지 않았다. 최종 알고리즘 채택이나 성능 측정 허락과 구분한다.

## 추천 묶음

| 항목 | 제안 동작 | 비용·한계/대안 |
| --- | --- | --- |
| 시간 단위 | CLOCK_MONOTONIC, 내부 정수 마이크로초. 0으로 계산된 유효 구간은 점수용으로1us. 역행/범위오류는 진단하고 표본으로 쓰지 않음 | 부동소수 상태/합계 누적 없음. 실제 클록 정밀도 보장 아님 |
| 대표값 | 첫 유효 표본을 초기 대표값으로 사용. 이후 EWMA old+(sample-old)/8, 부호별 정수 차분으로 overflow 방지 | 최근 변화 반영, α=1/8 임시값. 최근N창은 더 많은 기록 필요, 전체 산술평균은 오래된 이력 영향 |
| 초기 미측정 | 시간/결합정책에서 미측정 대상이 있으면 그 대상들 안에서 RR로 선택 | 초기 알려지지 않은 대상을 확인. 초기 느린 대상에 여러 요청이 겹칠 수 있음. 초기 probing 동시1건 제한은 별도 복잡도라 자동 추가하지 않음 |
| 오래된 값 | 일단 통계를 시간만으로 삭제하지 않고 마지막 갱신 시각 보존. 시간/결합정책은 전체 선택32회마다1회를 정상 가용 대상 전체 RR 탐색으로 사용 | 느려서 선택되지 않던 대상의 회복 표본 기회를 제공. 주기32는 임시값, 트래픽이 없으면 탐색도 없음. 더빠른회복 보장없음. TTL/자동제외 아님 |
| 정상/오류 | 정상 HTTP framing완료는 상태코드에 관계없이 실제 소요시간 표본. connect/전송/프레이밍오류·timeout은 max(실제경과,1초)로 점수용 실패표본. client취소·worker사망은 시간표본 제외, 예약은회수 | 빠른연결실패가빠른성공처럼선택되는것을완화. 1초는실패페널티임시값이며deadline을변경하지않음. 정상framing의HTTP500도건강판정으로쓰지않으므로빠를수있음. HTTP상태별health정책은추가하지않음 |
| worker 표본 | 정상전달/클라이언트가받은HTTP오류응답도연결처리를완료하면소요시간반영. RST/client취소/worker사망은제외. worker자체시스템처리실패는같은1초최소페널티 | backend의HTTP502라도worker가정상오류응답완료했다면worker자체장애로중복페널티하지않음. 두층결과를구분 |
| 네 정책 | RR; 최소outstanding; 최소대표시간; 최소(outstanding+1)×대표시간 | 결합은완료이력의요청비용추정일뿐현재요청의실제잔여비용을알지못함. 포화정수곱셈으로overflow방지,포화끼리는동률 |
| 동률과 순번 | 정수점수 정확일치만동률. 전체공유cursor에서가용후보를순환검색해최소값대상중선택. 선택후cursor=선택다음으로갱신 | 허용오차없음,근소차도반영. RR/탐색/동률은같은층cursor사용,worker/backend는분리 |
| 후보 적격 | worker는기존RUNNING·전달가능·C상한/active frame 조건을만족해야함. backend 조회A/B는둘다유지 | 실패이력으로backend를자동제외/우회하지않음. 선택된backend실패시같은요청재시도없음. 제출은고정backend |
| 실행 입력 | 기존필수proxy.conf에worker-policy와query-policy를각각필수추가, 값rr/least-outstanding/response-time/combined. 누락·중복·오타는listen전거절 | 묵시적기본알고리즘없음. 기존config/fixture도선택값을명시하도록변경필요. CLI구조는유지. 선택값은실행중자동전환하지않음 |

32회 탐색은시간/결합정책에만적용한다. 따라서표본이있는상태에서그두정책의31회는점수선택,32번째는탐색선택이다. RR/Least Outstanding에는자동탐색을섞지않는다. 초기미측정우선과정기탐색의우선순위는‘32번째탐색→그외미측정RR→그외점수’다. 탐색도동일공유잠금에서예약과 카운터갱신한다. worker/backend선택 횟수는 독립적으로관리한다.

오래된값을없애지않는만큼트래픽이낮거나실패가계속되는대상은회복표본이늦을수있다. 탐색없이최소값만고르면이문제가더지속될수있어32회탐색을명시적선택안으로포함했다. 이행동은아직승인되거나구현되지않았다.

## 책임·복구·저장 공간

backend의대표시간/최근갱신/표본유무/선택횟수·cursor는shared mmap이소유한다. 시간표본갱신과해제를같은transaction에두고before-image복구범위를확대해worker사망후중복표본·누락을검증한다. timestamp/소요시간계산과OSclock호출은lock밖에서실행하고lock안에서는검증된숫자만갱신한다. 같은backend의늦은완료가더오래된종료시각을전달해도마지막갱신시각을역행시키지않는다. EWMA갱신순서는완료lock획득순서다.

worker통계는parent가소유하고CLOSED에worker측소요시간/결과를추가한다. 기존세대·토큰검증과정확1회해제를통과한통지만표본에반영한다. early CLOSED는표본을중복적용하지않도록parent장부에보관한다. FD인계/일반완료frame version변경과tests를함께갱신한다.

대표값/최종갱신을각uint64와status/sample유무의고정 구조에 담는案이며N개rawsample은쌓지않는다. worker4/backend3에서는고정수백바이트규모의 추가를예상하나undo/earlyCLOSED/frame/connection시각까지합친정확비용은sizeof후보고한다. 초기reserve/완료·복구 각 단계SIGKILL검증을 다시 실행한다. 새global가변 상태/IO-under-lock/범용plugin은만들지않는다.

## 검증·비교 경계

코드전테스트기준을누적: EWMA첫표본/증감/정수반올림/절삭;미측정/32회탐색/회복/동률cursor;실패페널티·HTTP500/취소분리;곱overflow/시간역행;worker/backend상태격리;config오류;frame변조·중복/과거세대;시간통계transaction중/복구중SIGKILL·중복표본없음. 합의한 기능 단위마다 일반 컴파일과 ASan/UBSan 단위를 구분해 보고한다.

정상·느린backend·요청비용차이·장애·회복의실제부하비교는별도조건/지표/기간/반복/환경승인후진행한다. 이번묶음선택도성능측정실행허락이아니다. 제품main/HTTPworker미통합상태를유지하고완료됐다고표현하지않는다.

## 분배 규칙 묶음 구현 승인 (2026-11-25)
사용자 ‘구현’: EWMA1/8, 미측정RR, 시간/결합32회마다RR탐색, 실패min1초·client취소/사망표본제외, 결합(outstanding+1)*시간/포화, 정확동률RR, worker/query정책설정필수명시를채택했다. 최종알고리즘/성능실행은미확정이다. 공통계산→shared transaction→CLOSED parent통계→config순으로연결한다. 제품main의HTTP nonblocking구현완료를의미하지않는다.

## 네 분배 정책 구현·단위 검증 완료 (2026-11-25)
공통 distribution_policy(네정책/EWMA1/8/32회탐색/실패floor1초/포화곱/정확동률), sharedv4 통계+선택예약/완료해제 transaction과복구checksum, backend고정시간구간, parent관측CLOSEDv2(56byte)/선택배정, 필수worker-policy/query-policy설정 구현. 순차query경로도정책/표본연결;worker-policy의실제multiworker main연결은아직미통합. 예제config/proxy.conf.example 추가, 개인build설정불변. 일반O2/make all·ASan/UBSan전체18개통과. 통계변경10·선택변경8경계SIGKILL/재복구/2프로세스경합 검증. sharedmapping33528bytes(W4/R1024),상세비용/함수흐름 iteration-5-policy-review.md. 제품make verify/E2E·부하/성능·커밋/머지/푸시없음. 최종알고리즘미정유지.
