# Iteration 3 동적 서빙·CGI·청크 전송 테스트 기준

이 문서는 개인 브랜치의 동적 서빙(`GET /greet`), 외부 CGI 실행(`GET /cgi-bin/...`), HTTP/1.1 청크 전송(`Transfer-Encoding: chunked`)에서 검증해야 할 동작과 경계 사례를 정의한다. 실행 결과나 구현 완료 상태는 기록하지 않는다.

## 범위와 판정 원칙

- 이 문서는 [iteration-3-dynamic-requirements.md](iteration-3-dynamic-requirements.md)의 외부 계약과 [iteration-3-dynamic-design.md](iteration-3-dynamic-design.md)의 내부 전송·제공자 설계 계약을 검증하기 위한 기준이며, 정적 서빙 기준([iteration-3-tests.md](iteration-3-tests.md))과 함께 Iteration 3 개인 브랜치 검증을 구성한다.
- 외부 동작 테스트는 성공·오류 결과와 관찰 가능한 계약을 우선하며, 아직 구현 전인 내부 함수 이름, 버퍼 크기, 세부 HTML 마크업 문구는 계약상 명시된 항목을 제외하고 고정하지 않는다.
- 문서에 표기한 기존 코드 식별자(`http_parse_request()`, `parse_start_line()`, `http_select_handler()`, `select_rule()`, `router_prepare_error()`, `send_response()`, `send_body()`, `send_prepared_response()`, `body_provider.next_func`, `body_provider.cleanup_func` 등)는 코드 위치를 찾기 위한 표기이며, 이름 자체를 변경 불가능한 테스트 조건으로 만들지 않는다.
- 단위 검증(`make test`, ASan·UBSan)과 실제 `./server` 프로세스를 통한 E2E 검증을 구분한다.

---

## 1. 요청 파싱(`query` 분리)과 라우팅 — `parse_start_line()`, `select_rule()`

- `parse_start_line()`은 `request->target`에서 첫 번째 `?` 기준으로 `request->path`와 `request->query`를 분리한다.
  - `?`가 없으면 `request->query.length == 0`이다 (`/greet`).
  - `?` 뒤가 비어 있으면 `request->query.length == 0`이다 (`/greet?`).
  - 쿼리 안에 `?`가 다시 등장해도 첫 번째 `?` 이후 전체가 `request->query`에 포함된다 (`/greet?name=a?b`).
- `http_select_handler()`와 `select_rule()`은 디코딩하지 않은 원본 `request->path` 기준으로 라우트를 선택한다.
  - `GET /greet`는 정확 일치(`ROUTE_EXACT`)로 선택되며, `/greet/extra` 및 `/%67reet`는 `404`, `POST /greet`는 `405` 및 `Allow: GET`, `DELETE /greet`는 `501`이다.
  - `GET /cgi-bin/...`는 `/cgi-bin/` 접두사(`ROUTE_PREFIX`)로 선택되며, `/cgi-bin` 자체와 `/%63gi-bin/...`는 `404`, `POST /cgi-bin/...`는 `405` 및 `Allow: GET`이다.
- 기존 `/`, `/hello`, `/echo`, `/cafe`, `/menu`, `/assets/...`의 라우팅·동작·오류 코드가 회귀하지 않는다.

---

## 2. 내부 동적 핸들러 (`GET /greet`)

### Query 파싱 및 디코딩 경계

- `&`와 각 세그먼트의 첫 번째 `=`로 먼저 분리한 뒤, key와 value 모두에 대해 `+` → `' '` 치환 및 `%XX` 퍼센트 디코딩을 수행한다.
  - 인코딩된 key(`?%6E%61%6D%65=Alice`)를 `"name"`으로 인식한다.
  - 값 안의 인코딩된 구분자(`?name=A%26B%3DC`)가 파라미터 경계를 깨지 않고 `"A&B=C"`로 추출된다.
  - 값에 `=`가 여러 번 나오면(`?name=a=b`) 첫 번째 `=` 이후 전체 `"a=b"`를 값으로 유지한다.
  - 빈 세그먼트(`?&&name=Alice&&`)를 무시하고 정상 추출한다.
  - 무관한 파라미터(`?foo=1&name=Alice&bar=2`)가 함께 있어도 정상 추출한다.
  - key는 대소문자를 구별한다 (`?Name=Alice`는 `name`과 불일치).
- 중복 `name` 키는 WHATWG `URLSearchParams.get()` 규칙에 따라 **첫 번째 매칭 값**을 사용한다.
  - `/greet?name=Alice&name=Bob` → `"Alice"`
  - `/greet?name&name=Alice` → 첫 번째 `"name"`의 빈 값 `""`가 선택되어 기본 호칭으로 폴백한다.
- 방어적으로 Query 파싱 헬퍼가 직접 불완전한 `%` 구문(`%`, `%2`, `%ZZ`)을 전달받으면 `400`으로 처리한다.

### `%00`, 제어 문자, 누락·공백·길이

- 쿼리 내 **어느 파라미터의 key 또는 value에든** `%00`(NUL)이 포함되면 `400 Bad Request`로 거부한다 (예: `/greet?name=Alice%00`, `/greet?other=%00&name=Alice`, `/greet?na%00me=Alice`).
- 추출된 `name` 값에 일반 제어 문자(`0x01`~`0x1F`, `0x7F`, 예: `%09`, `%0A`, `%0D`, `%1B`)가 포함되면 `400 Bad Request`로 거부한다. (다른 미사용 파라미터에 일반 제어 문자가 있는 것만으로는 파싱 계층이 차단하지 않는다.)
- `name` 값의 앞뒤 ASCII 공백(`' '`)은 trim한다 (`?name=+Alice+` → `"Alice"`).
- `name` 누락(`/greet`, `/greet?`, `/greet?foo=bar`), 빈 값(`/greet?name`, `/greet?name=`), trim 후 빈 문자열(`/greet?name=%20%20`, `/greet?name=+`)은 모두 기본 호칭 **`"손님"`**으로 폴백해 `200 OK`를 반환한다.
- `GET /greet`에 `Content-Length > 0` 요청 본문이 함께 와도 본문을 무시하고 Query 기준으로 응답한다.
- `REQ_HEADER_MAX_BYTES` 이내의 긴 `name`(HTML 이스케이프로 길이가 크게 늘어나는 입력 포함)도 버퍼 잘림 없이 처리한다.
- 핸들러 내부의 `400`/`500` 오류는 기존 `router_prepare_error()`(`text/plain`) 형식을 유지한다.

### 응답 형식·XSS 이스케이프·동적 버퍼 정리

- 정상 응답 헤더는 `Content-Type: text/html; charset=utf-8`을 포함한다.
- `&`, `<`, `>`, `"`, `'` 5문자를 각각 `&amp;`, `&lt;`, `&gt;`, `&quot;`, `&#39;`로 치환한다 (`%3Cscript%3Ealert(1)%3C%2Fscript%3E`가 원문 `<script>` 태그로 출력되지 않음을 확인).
- 유효한 UTF-8 바이트(`%ED%99%8D%EA%B8%B8%EB%8F%99` 등)와 유효하지 않은 UTF-8 바이트(`%FF%FE` 등) 모두 5개 HTML 특수문자 외에는 바이트 단위로 손실 없이 출력된다.
- 동적 할당된 응답 본문 버퍼는 `body_provider.cleanup_func`를 통해 전송 성공·헤더 전송 실패·본문 전송 실패 경로 모두에서 누수 없이 해제된다 (ASan 검증).

---

## 3. 외부 CGI 실행 (`GET /cgi-bin/...`)

### 시작 조건 및 경로·보안 경계

- 작업 디렉터리에 `cgi-bin` 디렉터리가 없어도 서버는 정상 시작되며, 이 상태에서 `/cgi-bin/...` 요청만 `404`를 반환하고 기존 정적·동적 라우트는 정상 동작한다.
- `/cgi-bin/...` 경로에 유효한 `%` 인코딩이 포함되면 `400`이다.
- 빈 스크립트명(`/cgi-bin/`), 하위 디렉터리 또는 `PATH_INFO`(`/cgi-bin/sub/app`, `/cgi-bin/order/extra`), `..` 탈출(`/cgi-bin/../public/pages/cafe.html`), 외부 심볼릭 링크, 형제 디렉터리(`cgi-bin-old/`), 존재하지 않는 파일, 일반 파일이 아닌 대상(디렉터리·FIFO), 실행 권한이 없는 파일은 모두 개인 정책 `404`로 거부한다.

### 프로세스 실행·프로토콜·오류·자원 회수

- 유효한 CGI 요청 시 `REQUEST_METHOD=GET`, `QUERY_STRING`(원본 query 전달), `SCRIPT_NAME`, `SERVER_PROTOCOL=HTTP/1.1` 등 격리된 최소 환경변수만 전달되며 부모 환경의 임의 변수는 전달되지 않는다.
- 자식 프로세스로 부모의 `listenfd`/`connfd`(`FD_CLOEXEC`)가 상속되지 않고, `SIGPIPE`는 자식에서 `SIG_DFL`로 복원된다.
- CGI 출력 헤더가 `\r\n\r\n`으로 끝나든 `\n\n`으로 끝나든 모두 정상 파싱하며, 헤더와 한 번에 동반 도착한 본문 첫 조각도 누락 없이 첫 청크로 전송한다.
- CGI의 `Content-Type`을 응답 `Content-Type`으로 반영하며, 선택적 `Status:` 헤더가 유효한 상태(200~599, 204·304 제외)면 그 상태 코드를 사용한다. CGI가 출력한 `Content-Length`·`Transfer-Encoding`·`Connection` 등 프레이밍 헤더는 HTTP 응답 헤더에 그대로 전달하지 않는다.
- CGI 실패 분류:
  - `execve` 실패(예: 잘못된 shebang), 빈 stdout, 헤더 구분자 없이 상한 초과, `Content-Type` 누락·제어문자 포함, 잘못된 `Status:`, 헤더 단계 타임아웃(5초), 헤더 출력 없이 비정상 종료(`exit != 0` / 시그널)는 모두 **`502 Bad Gateway`**를 반환한다.
  - `malloc`, `pipe()`, `fork()` 등 서버 내부 자원 실패는 **`500 Internal Server Error`**를 반환한다.
- 본문 스트리밍 도중 자식이 비정상 종료(`exit != 0` / 시그널)하거나 파이프 타임아웃이 발생하면 `0\r\n\r\n` 종료 청크를 보내지 않고 전송을 중단한다.
- 정상 종료·오류·타임아웃·클라이언트 조기 종료 모든 경로에서 자식 프로세스를 `waitpid()`로 회수해 좀비 프로세스가 남지 않고, 열린 파이프 FD와 메모리가 누수되지 않는다.
- 데모 `/cgi-bin/order`: [cafe.html](../../../public/pages/cafe.html)→`/greet`, [menu.html](../../../public/pages/menu.html)→`/cgi-bin/order?item=...&qty=...`의 정상 견적(`200`, `Transfer-Encoding: chunked`)과 잘못된 항목/수량(`Status: 400`)이 동작하며, 정적 스크립트 `/assets/cafe.js`(`text/javascript`)가 제공된다.

---

## 4. 청크 전송 계약 (`Transfer-Encoding: chunked`) — `send_response()`, `send_prepared_response()`, 청크 본문 전송 흐름

- `chunked` 모드로 준비된 응답은 `Content-Length` 헤더를 내보내지 않고 `Transfer-Encoding: chunked`와 `Connection: close`를 전송한다.
- 청크 본문 전송 흐름은 `body_provider.next_func`가 `BODY_DATA`로 반환한 각 조각을 `<16진수 길이>\r\n<데이터>\r\n` 프레임으로 순서대로 전송하며, 현재 조각 전송 완료 전까지 다음 조각을 요청하지 않는다 (같은 버퍼를 재사용하는 제공자 포함).
- 여러 조각이 반환되면 각각 유효한 청크 프레임으로 전송하고, `BODY_END`를 받았을 때만 종료 청크 `0\r\n\r\n`을 전송한다 (빈 본문 스트림이라도 `BODY_END` 시 `0\r\n\r\n`만 전송).
- `BODY_DATA`에서 `data == NULL` 또는 `length == 0`을 반환하거나, `BODY_ERROR`를 반환하거나, 소켓 송신에 실패하면 **`0\r\n\r\n`을 전송하지 않고** 즉시 오류를 반환한다.
- 헤더 전송 실패, 본문 청크 전송 실패, 제공자 오류, 클라이언트 조기 종료 모든 경로에서 `send_prepared_response()`가 `body_provider.cleanup_func`를 정확히 한 번 호출하며 원래 전송 오류(`errno`)를 보존한다.
- 기존 `chunked == false` 경로(`send_body()`)의 길이 검증 및 실패 처리 계약은 그대로 유지된다.
