# 정적 파일 서빙을 코드로 따라가기

## 이번에 바뀐 것

기존 응답 본문은 Hello 문자열 또는 요청 본문 메모리였다. 이제 파일 FD도 응답 본문의
출처가 된다. 소켓 연결·HTTP 파싱·요청 본문 수신을 다시 만드는 단계가 아니라, 요청한
파일을 준비하고 그 바이트를 기존 소켓 전송 흐름으로 이어 주는 단계다.

예제: GET /static/css/style.css?version=1 HTTP/1.1

## 1. main이 파일 루트를 한 번 연다

`./build/server 8080 ./public`은 public 디렉터리를 문서 루트로 사용한다.
세 번째 인자를 생략해도 public을 사용하므로 기존 포트-only 실행을 유지한다.

main → open_static_root → POSIX open(O_DIRECTORY) → rootfd.

rootfd는 경로 탐색의 기준이다. main이 서버 수명 동안 소유하고 handle_client에 빌려준다.
프로세스 전체의 현재 디렉터리를 chdir로 바꾸거나 요청마다 루트 문자열을 붙이지 않는다.
루트가 잘못됐으면 연결을 받기 전에 시작 실패로 끝낸다.

## 2. 기존 파서가 요청의 원래 경로를 잘라 준다

헤더 수신 후 http_parse_request가 요청 줄과 헤더를 해석한다.
request.path는 `/static/css/style.css`만 가리키고 `?version=1`은 포함하지 않는다.
이 값은 원본 헤더의 포인터+길이인 slice이며 문자열 끝의 NUL이 없다.
따라서 원본에 바로 strlen/strcpy를 사용하지 않는다.

## 3. 라우터는 파일을 열지 않고 정적 요청인지 판정한다

http_route_request는 기존 /, /hello, /echo 이후 /static/ 접두사를 확인한다.
GET이면 HTTP_ROUTE_STATIC, POST이면 405와 Allow: GET이다.
다른 미지원 메서드와 기존 경로 정책은 그대로다.
파일 존재 여부는 라우터가 판단하지 않으므로 순수 라우팅 단위 테스트를 유지한다.

## 4. prepare_static_file이 실제 파일을 준비한다

입력은 rootfd, request.path이고 출력은 static_file(fd, length, content_type)이다.
성공은 0, 실패는 HTTP 상태 코드를 반환하며 실패할 때 남는 파일 FD는 없다.

1. 접두사를 제외한 상대 경로를 별도 배열에 한 번 디코딩한다.
   css/style.css가 되고 원본 헤더는 바꾸지 않는다. /static/이면 index.html을 사용한다.
2. 디코딩 후 .과 .. 요소를 거부한다. %2e%2e도 같은 검사에 걸린다.
   +는 공백으로 바꾸지 않는다. NUL은 C 문자열 경계를 바꾸므로 거부한다.
3. 탐색용 루트 FD를 복제하고 openat으로 css 디렉터리를 연다.
4. css FD를 기준으로 style.css를 연다. 각 단계에 O_NOFOLLOW를 사용하므로
   중간 디렉터리의 symlink와 마지막 파일의 symlink를 모두 거부한다.
5. 열린 그 파일 FD에 fstat을 호출해 일반 파일인지와 크기를 확인한다.
   경로를 검사한 뒤 별도로 다시 열지 않아 파일명 교체에 의한 대상 변경을 줄인다.
6. 디코딩된 파일 확장자로 MIME을 고른다. CSS는 text/css; charset=utf-8이다.

openat은 FD를 기준으로 파일을 여는 POSIX 함수다. prepare_static_file은 이런
시스템 호출과 프로젝트 정책을 묶은 직접 구현 함수다. 마지막 파일은 O_NONBLOCK으로
열어 FIFO에서 기다리지 않게 하고, 일반 파일이 아니면 전송하지 않는다.

## 5. 요청 본문과 Expect 규칙을 그대로 이어 간다

정적 파일 확인은 최종 헤더와 100 Continue보다 앞이다. 파일이 없으면 본문을 더
보내도록 요구하지 않고 404를 보낼 수 있다. 파일이 준비됐더라도 GET에 유효한
Content-Length가 있으면 기존 정책대로 선언 길이만큼 요청 본문을 먼저 받는다.
본문 수신이 실패하면 준비한 파일도 공통 cleanup에서 닫는다.

## 6. 헤더와 파일 본문을 분리해 전송한다

send_response_header는 메모리/파일 응답이 공유한다. 상태, MIME, 정확한 파일 길이,
Connection: close를 구성한다. 파일 길이는 uintmax_t로 전달해 size_t 축소를 피한다.

send_file_body는 16KiB 버퍼 하나를 반복 사용한다.

```text
파일 read → 실제 읽은 길이만 send_bytes → 남은 길이 감소 → 반복
```

send_bytes는 기존 부분 송신·종료 표시·호출별 송신 타임아웃을 처리한다.
HTML이든 PNG든 문자열 끝을 찾지 않고 바이트 길이로 전송하므로 NUL 때문에 이미지가
잘리지 않는다. 파일 전체를 malloc하지 않아 파일 크기에 비례하는 메모리가 필요 없다.
요청 본문의 64KiB 제한도 응답 파일에 적용하지 않는다.

처음 확인한 크기까지만 보내므로 전송 중 파일이 커져도 뒤 데이터를 덧붙이지 않는다.
파일이 줄어 조기 EOF가 나거나 읽기가 실패하면 이미 보낸 200 뒤에 새 500을 붙일 수
없으므로 연결을 종료한다. 파일이 제자리에서 수정될 때 내용 전체의 스냅샷은 보장하지 않는다.

## 7. 오류와 정리 지점을 확인한다

| 상황 | 상태/동작 |
| --- | --- |
| 잘못된 디코딩, NUL, 제어 문자, 역슬래시, 빈 경로 요소 | 400 |
| 점 경로 요소, symlink, 디렉터리/특수 파일, 권한 거부 | 403 |
| 파일/중간 디렉터리 없음, 중간 요소가 일반 파일 | 404 |
| 정적 경로에 POST | 405 + Allow: GET |
| 파일 열기 자원 부족 등 내부 오류 | 500 |
| 응답 헤더 전송 후 파일 읽기/전송 실패 | 추가 HTTP 응답 없이 연결 정리 |

정상 전송 후 기존 finish_response로 쓰기 방향을 닫고 제한적으로 남은 입력을 폐기한다.
이후 cleanup에서 file.fd를 닫고 요청 body를 free한다. main으로 돌아와 connfd를 닫는다.

| 자원 | 소유자 | 종료 지점 |
| --- | --- | --- |
| rootfd | main | 서버 종료/시작 실패 경로 |
| connfd | main | handle_client 복귀 뒤 |
| 탐색 중 디렉터리 FD | prepare_static_file | 다음 요소로 이동/실패 |
| file.fd | handle_client | 공통 cleanup |
| 요청 본문 body | handle_client | 공통 cleanup |

## 8. 검증에서 보는 것

화면 표시만으로 판단하지 않는다. MIME, Content-Length와 실제 수신 바이트 수,
이미지/큰 파일의 원본 일치, 오류 후 다음 정상 요청 처리를 확인한다.
403은 chmod 000 파일을 실제 비루트 서버로 읽어 검사한다. 원문 .. 경로는 브라우저나
curl이 먼저 정리할 수 있어 원시 소켓 또는 curl --path-as-is를 사용한다.

검증 실행 결과는 별도 iteration-3-tests.md에 기록한다. 테스트 파일의 존재와
컴파일 성공만으로 실제 기능 검증 통과를 의미하지 않는다.
