# P1-6 요청·중간 응답 헤더 보완 결과

날짜: 2026-11-25. Iteration4/Proxy-YYJ에서 기존 미커밋 P0-2·P1-4 변경을 보존하고 추가했다. main 머지·커밋·푸시는 수행하지 않았다.

## 실제 변경

src/proxy_transfer.c:
- forward_request_to_backend: 원래 Content-Length가 있거나 실제 본문이 있거나 POST인 경우 정확한 길이를 생성한다. 본문·길이 필드가 없는 GET에는 CL: 0을 합성하지 않는다. 명시적인 GET CL: 0은 유지한다. 빈 POST는 기존 계약대로 CL: 0을 생성한다.
- build_client_response_headers: 상태가 200 이상인 최종 응답에만 Connection: close를 생성한다. 중간 응답의 원래 hop-by-hop 필드는 계속 제거하고 Via·상태·전달 대상 헤더를 보존한다.
- 입력·본문 프레이밍·자원 소유권·메모리 할당 방식은 바꾸지 않는다.

실제 흐름은 main → handle_client → forward_request_to_backend와 receive_backend_response_headers → build_client_response_headers → send_prepared_response다. 새 콜백·파일 분리·성능 최적화는 추가하지 않았다.

## 검증 결과

| 구분 | 결과 |
| --- | --- |
| make clean | 성공 |
| make all | 컴파일·링크 성공 |
| make test | 7개 단위 바이너리 통과, ASan/UBSan 오류 보고 없음 |
| make verify | E2E 5개 통과 |
| Python 구문 확인 | verify_iter4.py 성공 |
| 최종 기능 테스트 실패 / skip | 없음 / 없음 |
| 사용자 설정 | clean·빌드 전후 SHA256 동일 |

단위 테스트는 실제 socketpair로 forward_request_to_backend가 보낸 헤더·본문을 받아 재파싱한다. GET의 길이 없음/명시적 0/3바이트 및 POST의 길이 없음/명시적 0/3바이트에서 길이 필드 유무·값, 바이너리 본문, Host 재작성·Via·close를 확인했다.

102/103/199의 중간 응답은 close·CL·TE가 없고 Link·Via를 보존하는지 확인했다. 원래 Connection에 지명된 필드는 제거하며 최종 200에는 close를 생성한다.

E2E는 C 백엔드가 수신한 Content-Length를 테스트 전용 X-Received-Content-Length 응답 헤더로 확인했다. GET의 absent와 명시적 0/3, POST의 0/3을 확인하고 응답 상태·바이트도 검사했다. 원래 103 블록과 최종 200 블록을 나눠 중간에는 Connection이 없고 최종에는 close가 있음을 확인했다.

기존 P0-2의 조기 400·백엔드 접속 0건, HTTP/1.0 고정 길이/EOF·204/304·상태 문구·오류, 정적/동적/CGI, 502/504·중도 종료·FD 회수·SIGINT/SIGTERM 회귀도 통과했다.

로그:
- build/p16-unit.log
- build/p16-e2e.log

## 보존 및 미검증

Host 재작성, 설정 파일 필수, 임시 타임아웃·Round-Robin·소유권·청크/트레일러를 유지한다. 실제 응답 백엔드는 C 코드이며 Python HTTP 서버를 추가하지 않았다. 파일·권한 변경은 임시 fixture로 한정했다.

부하·성능 및 S1/S_MAX 유지, 외부 교차 스위트 전체, 광범위한 퍼징/경합·모든 실패 주입, 실제 운영 ACL은 재측정/검증하지 않았다. 일반 E2E 바이너리는 -O2이고 단위 sanitizer와 구분한다. 전체 HTTP 준수 또는 성능을 보장하는 결과로 확대하지 않는다.
