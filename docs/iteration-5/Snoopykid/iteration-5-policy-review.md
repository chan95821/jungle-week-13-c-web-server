# Iteration 5 네 분배 정책 구현 리뷰

2026-11-25. 사용자가 시간 구간1번과 세부 규칙 묶음의 구현을 승인했다. 공통 정책 계산, backend 공유 통계 transaction, parent 작업자 선택·CLOSED 통계, 필수 설정을 구현했다. 최종 알고리즘은 정하지 않았다. 현재 제품 main은 순차 서버이며 실제 여러 worker의 HTTP 이벤트 루프 통합은 남아 있다.

## 설정과 현재 실행 계약

기존 ./build/server <port> <proxy.conf> 형식은 유지한다. 설정에는 주소3개 외에 아래 두 줄이 각각 필수다.

```
worker-policy rr
query-policy rr
```

각 값은 rr / least-outstanding / response-time / combined 중 하나다. 두 층에 다른 정책을 설정할 수 있다. 누락·중복·오타는 listen 전 설정 단계에서 거절한다. 예제의 RR은 최종 알고리즘 선정이 아니다.

config/proxy.conf.example을 추가하고 README와 회귀용 임시 설정 생성기를 갱신했다. 사용자의 build/proxy.conf.example은 수정하지 않았다. 기존 개인 설정을 계속 사용하려면 두 정책을 명시해야 한다. 새 체크아웃에서 실행할 예시는 ./build/server 8080 config/proxy.conf.example이다.

query-policy는 기존 순차 경로에도 연결했다. worker-policy는 실제 프로세스 풀 통합에서 사용할 설정이며 현재 제품 main은 작업자를 생성하지 않는다. 순차 서버에서 Least Outstanding는 각 새 요청 시 진행 수가 같으므로 RR처럼 동작한다. 이 경로로 동시성 모델의 성능 차이를 평가하면 안 된다.

## main에서 하위 함수로 이어지는 흐름

### 현재 순차 실행

main → proxy_load_config → accept → handle_client → http_select_route / proxy_validate_request → 요청 본문 완료 → proxy_select_backend → proxy_prepare_tracked_response → read_backend_body_chunk → finish_proxy_timing → 기존 client 출력·정리.

proxy_select_backend가 공통 choose_distribution_candidate를 호출한다. query 상태는 main이 보유한 기존 selection 안에 있으며 가변 전역을 추가하지 않았다. proxy_prepare_tracked_response는 선택된 query timing을 빌려 connect 직전 CLOCK_MONOTONIC을 기록한다. read_backend_body_chunk가 고정 길이의 마지막 바이트, 검증된 청크 종료+trailer, 정상 EOF를 확인하면 표본을 한 번 반영한다. 0바이트/무본문은 최종 헤더 검증 후 반영한다. 실제 clock 실패는 표본을 생략하고 기존 HTTP 응답을 바꾸지 않는다. 연결/프레이밍 실패는 점수용 실패 표본, client 취소는 제외한다. HTTP500도 framing이 정상 완료되면 정상 소요시간 표본이다.

### 프로세스 작업자용 연결점

parent: select_and_assign_parent_connection → 후보 적격·공통 정책 계산 → 배정 성공 후 parent cursor·탐색 횟수 게시. caller가 supervisor RUNNING·채널 전달 가능 여부를 available 배열로 제공한다. C 상한과 parent 세대도 검증한다.

worker: 공유 잠금 → begin_selected_query_request_locked → select_and_reserve_query_locked → 선택+요청 예약+전체 cursor/탐색 횟수를 같은 journal transaction으로 commit → 잠금 해제 → start_backend_request_clock(connect 직전 시각) → nonblocking I/O → finish_backend_request_clock(검증 완료/실패 시각) → try_release_backend_request → complete_backend_request_observed_locked.

complete_backend_request_observed_locked는 표본 갱신과 outstanding 감소를 같은 commit에 넣는다. EBUSY면 소요시간·종료시각·토큰을 고정 보존한다. 늦은 잠금 획득으로 응답 시간을 늘리지 않는다. 중복·과거 토큰은 표본도 다시 반영하지 않는다. 유효하지 않은 시간 구간은 오류를 반환하되 해제 pending으로 옮겨 예약 누수를 막고 유효 표본은 생략한다. caller가 숫자 오류를 보고해야 한다.

worker 완료: 작업자가 FD 인계 완료부터 자원 정리 완료까지 계산한 observation을 worker_connection_closed에 담는다 → bounded STREAM CLOSED v2 전송 → advance_worker_closed_receive → apply_worker_connection_observed_closed → 정상 세대/소유권/토큰의 정확1회 해제에서 parent worker 대표값 반영.

CLOSED가 SENDING 도중 먼저 오면 장부에 최초 표본을 보존하고 finish_parent_connection_handoff 뒤에 반영한다. 중복·이전 세대는 통계도 오염시키지 않는다. worker 사망 자체를 시간 표본으로 만들지 않는다. 이미 검증된 완료 통지가 먼저 도착한 경우 그 완료 표본은 유효하다. worker 표본의 실제 생성·완료 hook과 부모 dispatch 루프의 제품 연결은 아직 남아 있다.

## 공통 계산 규칙

- 내부 정수 microseconds, 0으로 계산된 유효 구간은 점수에서1us. 단조 timespec을 검증하고 경과 시간을 정수화한다. 실제 클록 해상도를 보장하는 값은 아니다.
- 대표값은 첫 표본 자체, 이후 old+(sample-old)/8이다. 증가·감소를 unsigned 차분으로 나눠 overflow를 피한다. 정수 나눗셈의 절삭을 사용한다.
- 정상 표본은 실제 경과시간, 시스템/backend 실패는 max(경과시간,1,000,000us). client 취소·사망은 표본을 만들지 않는다. 실패 페널티는 timeout을 변경하지 않는다.
- 시간/결합 정책: 32번째 선택은 전체 적격 후보 RR 탐색 → 나머지는 미측정 후보 RR → 나머지는 최소 점수. RR/Least Outstanding에는 탐색을 섞지 않는다.
- 점수: RR=동일, Least Outstanding=진행 수, 시간=대표값, 결합=(진행 수+1)×대표값. 곱 overflow는 UINT64_MAX로 포화하며 포화끼리도 동률이다.
- 후보는 공유 cursor부터 순환 검사하고 정확히 동일한 최소 점수에서 RR 순서를 적용한다. 선택 성공 후 cursor는 선택 다음, 탐색 횟수는32로 순환한다. worker와 backend의 상태·cursor·정책은 독립이다.
- 오래된 대표값을 삭제하지 않고 마지막 종료시각을 보존한다. 늦은 표본의 종료시각이 오래됐어도 마지막시각을 역행시키지 않는다. EWMA 순서는 완료 lock 획득 순서다. 탐색은 트래픽이 있을 때만 생기며 빠른 회복을 보장하지 않는다.

## shared 복구와 프레임 변경

shared layout version은4다. header에 backend별 representative/last_finished와 query 탐색 횟수를 추가했다. journal은 이전 통계 전체와 cursor/count를 before-image에 넣고 checksum에도 포함한다. PREPARED는 이전 통계와 카운터를 같이 복원, COMMITTED는 같이 보존한다. 통계/순번이 공유 잠금 밖에서 갱신되지 않는다. clock 호출·네트워크/파일 I/O도 잠금 안에서 하지 않는다.

대표값과 종료시각 사이, query cursor와 탐색 횟수 사이에 실제 사망 주입 경계를 추가했다. 복구 중 대표값을 복원한 직후 재사망해도 다음 복구가 끝까지 다시 복원한다. robust mutex가 임의 메모리 손상을 모두 탐지한다는 주장은 유지하지 않는다.

CLOSED 프레임 version2는56바이트다. 기존 식별자32바이트에 elapsed_us·finished_us·result·reserved를 담는다. 다른 프로세스에서 원시 시작/종료시각을 빼지 않으며 worker가 계산한 elapsed를 사용한다. finished는 최근 갱신시각 관리용이다. 구버전·잘못된 결과/시간·reserved·예상치 않은 FD는 거절한다. 개인 바이너리/worker 프로토콜의 혼합 실행은 지원하지 않는다.

## 컴파일·실제 검증 결과

| 범위 | 결과 |
| --- | --- |
| 제품 make all, 변경 구성요소의 테스트훅 없는 C11/Wall/Wextra/Werror/O2 컴파일 | 통과 |
| ASan/UBSan make test 전체18개 실행 파일 | 통과(exit0) |
| EWMA 첫값/증감/절삭·실패floor·취소·범위/시간역행·곱포화 | 통과 |
| 네 정책·정확동률·미측정·32번째탐색·후보없음 불변 | 통과 |
| 필수config 네정책 조합/누락/중복/미지원값, 실패 시 기존 config 보존 | 통과 |
| 시간표본+해제 10개 mutation 지점 실제 SIGKILL | 통과 |
| 선택+예약+cursor/count 8개 mutation 지점 실제 SIGKILL | 통과 |
| 변경 backend의 대표값 복원 직후 복구자 재사망→재복구, 시각·카운터 복원 | 통과 |
| 실제 두 프로세스의 32개 선택/예약/완료 경합·원장불변/통계보존 | 통과 |
| CLOSED v2 실제 STREAM·early/중복 표본 1회·미측정 worker 선택·세대회수 | 통과 |
| 프레임 구버전·결과/시간/예약필드 변조, 기존 부분I/O/FD누수 회귀 | 통과 |
| 고정길이·청크·EOF의 실제 body parser 표본 완료 지점, 실패/취소 분리 | 통과 |
| 기존 신호/큐/FD/원장/사망/재복구/프레이밍 회귀 | 통과 |

마지막 전체 로그는 build/iteration5-capability/unit-tests-with-policy.log다. 새 정책 단위의 강화된 대표값 복구 사망 시험은 build/iteration5-capability/distribution-policy-test.log에서 별도 재확인했다. 초기 실패는 공통 모듈 링크 누락/중복과 구프레임 길이·시험 stream의 body_started 누락이었다. 기대 동작을 약화하지 않고 실제 규약과 기존 초기화 조건에 맞춰 수정했다.

제품 make verify E2E는 이번 흐름에서 실행하지 않았다. 새 임시 config 생성기는 갱신했지만 서버 프로세스 기반 전체 회귀의 실행 결과로 표현하지 않는다. skip 처리한 단위는 없으며, E2E·부하·성능은 미검증/미실행이다. 통계 계산 검증과 일반 요청 경로용 clock 구현은 벤치마크 측정 결과가 아니다.

## 저장 공간과 한계

현재 WSL ABI에서 shared header344/journal184바이트, W4/R1024 mapping33,528바이트다(이전33,416보다112 증가). parent entry56, worker40, ledger owner72, 후보 scratch32/worker다. W4/G1152 parent 장부는64,872바이트다. CLOSED token48, queue owner104, receiver72이므로 W4/C256 큐49,568+수신288바이트, 장부와 합계114,728바이트다. backend lifetime은32→80바이트이며 최대1024개 가정81,920바이트다. 모든 값은 sizeof/설정으로 계산한 저장 공간이며 실제 서버 전체 메모리 측정이 아니다. 기존 FIFO·supervisor·HTTP/프록시·파일/CGI·kernel socket buffer 등은 별도다.

최종 알고리즘과 성능 우위는 미정이다. 자동 재시도·장애 backend 제외·알고리즘 전환을 추가하지 않았다. worker/backend 실제 nonblocking HTTP 루프, worker lock 재시도·역압력·메인 통합·CGI/파일/로그sink/부모사망 수명은 남아 있다. 커밋·머지·푸시하지 않았다.

다음 제품 E2E 검토 후보는 RR 명시 config의 기존 전체회귀, 누락/오타 시 listen 전 종료, 시간/결합 정책의 C backend 목적지와 바이너리/오류 응답 유지다. 부하·처리량·지연 비교는 별도 조건·지표·기간·반복·환경 허락 후 진행한다.
