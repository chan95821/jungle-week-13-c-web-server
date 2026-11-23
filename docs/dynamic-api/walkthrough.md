# 동적 API를 함수 순서로 읽기

## 정적과 동적을 나눈 위치

- static_file.c: 디스크 파일을 안전하게 열고 파일 정보/MIME 준비.
- dynamic.c: 요청 값을 해석하고 계산해서 JSON 본문 생성.
- router.c: 요청을 어느 처리로 보낼지 판정.
- server.c: 수신 완료를 보장하고 준비된 응답을 전송하며 자원 정리.

정적/동적을 각각 다른 서버로 실행하는 구조가 아니라, 같은 서버의 서로 다른
처리 모듈이다. 기존 정적 샘플 페이지와 /hello, /echo 동작은 유지한다.

## 1. 라우터가 처리 종류를 정한다

GET /api/time이면 HTTP_ROUTE_TIME, POST /api/add면 HTTP_ROUTE_ADD다.
경로가 맞아도 GET/POST가 다르면 405와 Allow로 거부한다.
라우터 안에서 시간을 조회하거나 덧셈을 실행하지 않는다.

## 2. 본문을 받기 전에 헤더 조건을 확인한다

http_parse_request는 Content-Type 값을 원본 헤더의 slice로 보관한다.
값이 두 번 나왔는지도 표시하지만 API별 정책은 강제하지 않는다.
따라서 /echo는 이전처럼 타입을 무시한다.

handle_client → dynamic_check_headers.

/api/add는 form 입력만 지원하므로 다른 타입/누락은 415, 중복은 400이다.
이 검사는 100 Continue와 추가 본문 수신 전에 실행한다. 이미 헤더만으로 거부할 수
있는 요청에 본문을 더 보내라고 요구하지 않기 위해서다.

## 3. 기존 코드로 요청 본문을 완성한다

헤더와 함께 받은 본문을 보존하고 Content-Length까지 부족한 부분만 읽는다.
64KiB 상한과 수신 기한 및 종료 표시를 그대로 적용한다.
본문이 덜 왔거나 오류가 나면 계산 함수를 호출하지 않는다.

## 4. 덧셈 입력을 해석한다

예: a=10&b=-3

prepare_dynamic_response → parse_add_parameters → decode_form_component → parse_integer.

- parse_add_parameters: 원본 body를 길이 기준으로 &와 = 위치에서 나눈다.
- decode_form_component: 키/값을 한 번 디코딩한다. +는 공백이므로 숫자의 + 부호는 %2B로 보낸다.
- parse_integer: 부호와 숫자를 읽고 곱셈 전에 범위를 검사한다.

키는 a/b를 각각 하나씩만 받고 누락/중복/기타 키는 400이다. 숫자의 작업 공간은
종료 문자 포함 32바이트이며, 일반적인 64비트 정수 표기는 부호 포함 최대 20바이트다.
NUL, 제어 문자, 소수, 문자열, 정수 범위 초과도 거부한다.

INT64_MIN의 절댓값은 양의 int64_t로 표현할 수 없다. 그래서 크기는 uint64_t로
누적하고 마지막에 부호를 적용한다. 덧셈은 실행하기 전에 INT64_MAX/MIN 경계를
검사한다. C에서 signed 오버플로우가 발생한 뒤 검사하는 방식은 사용하지 않는다.

## 5. 시간 조회 또는 계산 결과로 JSON을 만든다

/api/time은 표준 C time(NULL)로 현재 Unix 초를 얻는다. 타임아웃에 사용하는 기존
CLOCK_MONOTONIC은 경과 시간용이므로 사용자에게 보여 줄 시각으로 쓰지 않는다.

/api/add는 a+b를 JSON의 정수로 넣는다.

```text
{"timestamp":현재초}\n
{"result":7}\n
```

snprintf가 쓴 길이를 확인하고 그 길이를 response.length에 넣는다.
종료 NUL은 전송하지 않고 실제 JSON과 마지막 개행만 보낸다.
숫자만 삽입하므로 사용자 문자열을 JSON 안에 그대로 끼워 넣지 않는다.

## 6. 응답 메모리는 호출자가 소유한다

handle_client는 dynamic_body[256]을 가진다. prepare_dynamic_response에 이 배열을
넘기면 함수가 내용을 채우고 response.body가 그 버퍼를 가리키게 한다.

```text
handle_client의 dynamic_body ← JSON 생성
                    ↑
             response.body
                    ↓
             send_response
```

동적 함수 내부의 지역 배열 주소를 반환하면 함수 종료 뒤 포인터가 유효하지 않다.
지금은 호출자의 배열이므로 전송이 끝날 때까지 살아 있다. 새 malloc/free도 필요 없다.

## 7. 전송과 정리는 기존 함수를 쓴다

server.c → send_response → send_response_header + send_bytes.

정상 결과는 application/json이고 Content-Length는 실제 생성 바이트 수다.
동적 처리 오류는 기존 router_prepare_response로 text/plain HTTP 오류를 준비한다.
그 후 기존 연결 마무리·요청 본문 free·main의 연결 close 순서로 돌아간다.

## 현재 확인 상태

코드/상세 주석/API 문서를 작성했다. 빌드·서버 실행·기능 테스트는 아직 하지 않았다.
기존 Ubuntu WSL 장애와 테스트 실행 제한을 유지한다. 문서의 curl 출력은 사용 예시다.
