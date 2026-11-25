# P2-B 모듈 분리·버퍼 수명 개선 결과

날짜: 2026-11-25. 사용자가 B안을 선택해 파일 분리와 메모리 수명 개선을 함께 구현했다. Iteration4/Proxy-YYJ의 기존 미커밋 보완을 보존했으며 main 머지·커밋·푸시는 하지 않았다.

## 실제 변경과 소유권

| 파일 | 책임 |
| --- | --- |
| src/proxy_transfer.c | 상태 생성, 연결/전달/헤더 준비 조율, 소유권 이전, 정리 |
| src/proxy_io.c | 백엔드 연결, 전체/idle 기한, 버퍼 수신·바이트/줄 읽기 |
| src/proxy_headers.c | 요청 전달 헤더, 응답 파싱·재구성, 필요한 Connection 정보 복사 |
| src/proxy_body.c | CL/chunked/EOF 본문 공급, 출력 헤더 회수, 트레일러 검증·보존 |
| src/proxy_internal.h | 내부 상태·준비 작업 공간·모듈 간 함수 계약 |

main → handle_client → proxy_prepare_response → connect_backend → forward_request_to_backend → prepare_backend_response_headers → send_prepared_response → read_backend_body_chunk → cleanup_backend_response.

원본 헤더와 출력 재구성 배열은 헤더 준비의 지역 작업 공간으로 이동했다. 최종 chunked 응답의 Connection 필드만 필요한 길이로 보존하므로, 트레일러 검사 시 지역 원본을 참조하지 않는다. 반복 필드와 대소문자 구분 없는 지명 검사를 유지했다. 출력 헤더는 정확한 완성 길이만 할당하고 헤더 송신 후 첫 provider next에서 회수한다. 본문 없음·헤더 송신 실패는 cleanup이 회수한다. 트레일러도 유효한 비어 있지 않은 값이 있을 때 정확한 길이만 할당한다.

준비 단계 할당 실패는 정리 후 기존 500, 본문 중 할당 실패는 BODY_ERROR다. 헤더 송신이 시작된 뒤 새 오류 응답을 추가하지 않는다. 요청 본문·클라이언트 FD 소유권, Host 재작성·필수 설정·버전·기한·상한·라우팅/분배는 바꾸지 않았다.

## 컴파일·링크 확인

- make clean 후 make all backend 및 단위 실행 파일 8개 컴파일·링크 성공.
- C11, -Wall -Wextra -Werror, 서버 -O2 유지.
- 새 src 모듈은 build/obj 아래, 할당 주입 객체는 build/test-obj 아래 생성한다. clean 대상에도 등록했다.
- 할당 주입은 테스트 전용 객체에서만 malloc/calloc/free 이름을 치환한다. GNU 링커 전용 --wrap 옵션에 의존하지 않으며 제품 전역 상태·테스트 훅은 추가하지 않았다.

## 실행 검증 결과

| 검증 | 결과 |
| --- | --- |
| make test | ASan/UBSan 단위 실행 파일 8개 통과 |
| 수명 테스트 추가 보강 후 별도 재실행 | 통과; FD 비교·트레일러 8KiB 경계 포함 |
| make verify | E2E 스크립트 6개 통과 |
| 엣지 스위트 | 40개 통과, 실패 0, skip 0 |
| 최종 실패 / skip | 없음 / 없음 |

수명 테스트는 실제 C/POSIX 자식 백엔드와 public proxy_prepare_response를 사용한다. state/Connection/출력 헤더 할당 실패 500·정리, 첫 next에서 출력 헤더 회수, next 없는 0바이트/204/송신 실패 대응 cleanup, 다중 Connection 지명 트레일러 거절, 트레일러 할당 실패 BODY_ERROR, 빈 트레일러 무할당, 합계 8192바이트 성공·8194바이트 실패 및 FD 회수를 확인했다. FD 실패 주입 비교는 격리 테스트의 0~255 범위이며 전체 시스템 FD 보증은 아니다. 일반 회귀에서는 기존 반복 장애·클라이언트 RST·FD 누적 검증도 통과했다.

프로토콜 테스트는 이전처럼 .c 파일을 직접 include하지 않고 실제 분리 모듈과 링크한다. 기존 CL/EOF/chunked·트레일러·1xx·204/304·HTTP/1.0·원래 상태 문구·바이너리 정책의 회귀도 통과했다.

로그: build/p2-build.log, build/p2-unit.log, build/p2-lifecycle.log, build/p2-e2e.log.

## 메모리 설명과 한계

기존 고정 배열 합 49,408바이트(48.25KiB)에서 스트림의 고정 배열은 16,384바이트만 남았다. 실제 스트림에는 메타데이터도 있고, 처리 단계에 따라 출력 헤더(기존 상한 16,640바이트), Connection 필드(기존 원본 헤더 상한 내), 트레일러(최대 8KiB)가 추가된다. 원본 헤더/재구성 지역 배열 합은 24,832바이트(24.25KiB)이고 본문/트레일러의 지역 작업 공간도 있다. 이는 코드상 용량 설명이며 RSS/최대 메모리·CPU·처리량 개선 수치가 아니다. 할당 횟수와 복사 비용, 작업자 스택 예산을 별도로 고려해야 한다. 모든 경우의 메모리 50% 감소나 처리량 유지/개선을 보장하지 않는다.

새 수명은 현재 순차 blocking provider 계약을 따른다. Iteration 5 이벤트 루프에서 부분 헤더·송신 상태를 유지할 방법과 버퍼/역압력 정책은 다시 설계해야 한다. 1만 연결·macOS 실행·외부 교차 스위트 전체·성능/부하 측정은 이번에 실행하지 않았다. 실제 운영 정적 루트·권한을 바꾸지 않았으며 테스트는 임시 fixture를 사용했다.
