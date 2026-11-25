# P2-9 A안 — 라우팅 일원화 검증 결과

날짜: 2026-11-25. 사용자가 라우팅만 일원화하는 A안을 선택했다. Iteration4/Proxy-YYJ의 기존 보완을 보존했다. main 머지·커밋·푸시는 수행하지 않았다.

## 구현 범위

router.c의 routes[]에 기존 로컬 8개·프록시 3개 규칙을 함께 선언했다. http_select_route가 단일 진입점이며 request_route의 local_handler 또는 proxy_route를 선택한다. 라우팅 역할 enum은 router.h로 이동하고 실제 주소·Round-Robin 상태는 기존 proxy_config/proxy_selection_state에 유지한다.

proxy.c의 경로/메서드 if/else와 proxy_match_request는 제거했다. proxy_validate_request는 기존 보호 Connection 헤더 검사만 수행한다. handle_client는 파서 → 통합 라우터 → 성공한 프록시 요청의 보호 검사 → 본문/100 Continue → 기존 실행·중계 순서로 호출한다. 이전 http_select_handler도 제거했으며 로컬 응답 단위 테스트는 새 진입점을 사용하는 테스트 helper로 옮겼다. 새 검증 콜백·할당·I/O·공유 상태는 도입하지 않았다.

## 보존한 계약

- HTTP 파서 오류/413/431이 먼저 결정된다. 라우터는 미지원 메서드 501, 모르는 GET/POST 경로 404, 등록 경로의 잘못된 GET/POST 405·Allow를 유지한다.
- 라우팅 성공한 프록시 요청에만 보호 헤더 400을 적용한다. 로컬 경로는 기존대로 통과하고, 잘못된 메서드/미등록 경로에 보호 헤더를 넣어도 기존 라우팅 오류가 먼저다.
- 정확/접두사·최장 일치, 쿼리 분리 후 경로, 원래 target과 헤더를 변경하지 않는다. 실패 선택 결과는 LOCAL/NULL로 초기화된다.
- 본문/100/백엔드 연결보다 라우팅과 보호 검사가 앞선다. 주소 선택·순번 변경은 본문 완료 후 기존 위치에서만 수행한다.
- Host 재작성·필수 설정·HTTP 버전·임시 상한·기한·P2 버퍼 수명·FD 소유권은 유지한다.

## 컴파일·링크

make clean 후 make all backend와 단위 실행 파일 8개 빌드에 성공했다. C11·경고 오류 처리·서버 -O2와 ASan/UBSan 설정을 유지했다. Python 검증 스크립트 구문 확인도 성공했다.

## 실행 결과

| 검증 | 결과 |
| --- | --- |
| make test | ASan/UBSan 단위 실행 파일 8개 통과 |
| make verify | E2E 스크립트 6개 통과 |
| 엣지 스위트 | 40개 통과, 실패 0, skip 0 |
| 전체 최종 실패 / skip | 없음 / 없음 |
| 사용자 build/proxy.conf.example | clean·빌드 전후 SHA256 동일 |
| 테스트 프로세스 | server/test_backend 잔류 없음 |

라우터 단위 테스트는 모든 규칙과 접두사 경계, GET/POST/HEAD/OPTIONS/get, 미등록 경로, handler/역할/Allow와 원본 입력 보존을 확인했다. 로컬 응답 테스트도 동일한 새 라우터를 사용하며 정적·동적·CGI 응답을 계속 검증한다. 선택 테스트는 라우터 호출만으로 순번이 바뀌지 않음과 제출·조회 공유 RR·독립 상태를 확인했다.

E2E의 수동 TCP 접속 감지용 리스너 앞에서 추가 11개 오류 우선순위 사례를 실행했다. Content-Length: 10·Expect: 100-continue·Connection: Host를 넣고 본문을 보내지 않아도 405/501/404가 유지되고 100을 보내지 않는지, 파서의 413/400이 405보다 앞서는지 확인했다. Allow와 백엔드 접속 0건도 확인했다. 기존 정상 로컬 통과, 보호 헤더 조기 400, 정상 조회 A/B 순번 및 전체 회귀도 통과했다. 이 추가 사례는 기존 스크립트 내부의 세부 검사이며 edge 논리 사례 수 40에 별도로 더해 표기하지 않는다.

로그: build/routing-build.log, build/routing-unit.log, build/routing-e2e.log.

## 미검증·후속 범위

IPv6·가변 백엔드·선택 정책 추상화·새 설정 형식·동시성은 구현하지 않았다. 부하/성능·1만 연결·외부 교차 스위트 전체·다른 OS 실행은 검증하지 않았다. 테스트는 임시 fixture를 사용했고 실제 운영 정적 루트·권한을 바꾸지 않았다. 과거 보고서의 proxy_match_request/http_select_handler 설명은 그 당시 코드 기록으로 보존하며 현재 흐름은 walkthrough에 갱신했다.
