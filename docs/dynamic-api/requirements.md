# 동적 API 구현 범위

사용자 선택: GET /api/time + POST /api/add 두 기능. 현재 서버 내부에서 처리한다.

## API 계약

| 요청 | 정상 응답 |
| --- | --- |
| GET /api/time | 200, application/json, {"timestamp":초 단위 Unix 시각} |
| POST /api/add | form 본문 a=10&b=20 → 200, application/json, {"result":30} |

- add는 application/x-www-form-urlencoded Content-Type을 요구한다. 미디어 타입의 대소문자는 무시하고 세미콜론 이후 매개변수는 본문 해석에 사용하지 않는다.
- 두 숫자는 부호 있는 64비트 정수. 빈 값, 잘못된 숫자, 누락/중복/알 수 없는 키, NUL, 인코딩 오류, 입력/합계 범위 초과는 400.
- form 키/값은 한 번 percent decode하고 +는 공백으로 해석한다. 숫자의 양수 부호 +를 보내려면 %2B로 인코딩한다.
- Content-Type 누락/다른 형식은 415, 중복 Content-Type은 이 API에서 400. 기존 /echo의 타입 무시 정책은 유지한다.
- 잘못된 GET/POST 조합은 기존 405+Allow. HTTP 오류는 기존 text/plain 응답, 정상 동적 결과는 JSON.
- 기존 HTTP 요청 본문 길이, 부분 수신, 100 Continue, 정적 파일, 종료 정책을 바꾸지 않는다.
- 응답은 handle_client 소유의 256바이트 버퍼에 생성하며 전송 종료까지 유지한다. 새 전역 상태/동적 할당은 없다.
- 시각 조회와 문자열 생성 실패는 500. 입력은 원본 slice를 변경하지 않고 길이로 해석한다.

## 제외

CGI, 프록시, DB·영구 저장, 인증, JSON 요청 파서, 소수 계산, 범용 form 라이브러리,
광범위한 시험·성능 측정. 현재 테스트 실행 제한은 유지한다.
