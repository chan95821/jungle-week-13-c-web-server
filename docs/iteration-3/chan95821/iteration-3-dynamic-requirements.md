# Iteration 3 동적 서빙 요구사항

상태: 확정 (구현·검증 전). 이 문서는 개인 브랜치의 동적 서빙(서버 내부 동적 핸들러, 외부 CGI 실행, 청크 전송)에서 **외부적으로 관찰 가능한 기능·입출력 계약·오류·보안 요구사항**을 정의한다. 내부 C 구조와 구현 방법은 [iteration-3-dynamic-design.md](iteration-3-dynamic-design.md)에 분리해 기록한다.

## 기준 및 범위

- [Issue #3](https://github.com/chan95821/jungle-period/jungle-week-13-c-web-server/issues/3): 서버 내부 동적 응답 구현(`GET /greet?name=...`), 기존 `GET /hello`, `POST /echo` 및 정적 서빙 유지.
- 과제 원문([web-server.md](../../requirements/web-server.md)): 요청에 따라 애플리케이션 로직을 실행하고 응답 생성(CGI 프로그램 실행 포함).
- 정적 서빙 기준: [iteration-3-requirements.md](iteration-3-requirements.md)
- **범위 외**: Reverse Proxy / 백엔드 서버 전달(Iteration 4), `POST` 기반 CGI 본문 전달, CGI `PATH_INFO`·하위 디렉터리 실행, 범용 템플릿 엔진, 인증, 성능 측정.

---

## 1. 데모 시나리오

정글 카페 데모 안에서 정적 서빙·내부 동적 응답·외부 CGI 실행을 하나의 흐름으로 연결한다.

1. **카페 소개 ([public/pages/cafe.html](../../../public/pages/cafe.html), `GET /cafe`)**: 방문자 이름 입력 폼을 두고, 브라우저에서 정적 스크립트(`/assets/cafe.js`)가 `GET /greet?name=...` 결과를 비동기로 받아 화면에 반영한다. (JS 미사용·직접 접속 시에도 동일한 HTML 응답 제공)
2. **환영 페이지 (`GET /greet`)**: 입력한 이름을 포함한 개인화된 HTML 환영 인사를 반환하고, `/cafe`와 `/menu` 이동 링크를 제공한다.
3. **메뉴 및 주문 견적 ([public/pages/menu.html](../../../public/pages/menu.html), `GET /menu`)**: 메뉴 가격(아메리카노 4,000원, 카페 라테 4,500원, 허브 티 4,000원)과 품목(`item`)·수량(`qty`) 입력 폼을 두고, `/assets/cafe.js`가 `GET /cgi-bin/order?item=...&qty=...` 결과를 비동기로 받아 견적을 표시한다.
4. **주문 견적 CGI (`GET /cgi-bin/order`)**: 외부 실행 프로그램이 `item`과 `qty`(1~99)를 검증·계산해 견적 HTML(잘못된 입력 시 `400` 안내 HTML)을 출력한다.

---

## 2. 라우팅 및 URL 인코딩 경계

- 라우트 매칭은 디코딩하지 않은 원본 URI 경로를 기준으로 한다.
  - `GET /greet`: 정확 일치. `/greet/extra` 및 `/%67reet`는 `404`, `POST /greet`는 `405` + `Allow: GET`, 미지원 메서드는 `501`.
  - `GET /cgi-bin/`: 접두사 일치(단일 파일명만 허용). `/cgi-bin` 및 `/%63gi-bin/...`는 `404`, `POST /cgi-bin/...`는 `405` + `Allow: GET`, 미지원 메서드는 `501`.
- 경로 부분의 `%` 인코딩 정책은 핸들러 성격에 따른다.
  - `/cgi-bin/...` 경로 부분에 `%`가 포함되면 `400`으로 거부한다.
  - 동적 라우트의 Query 문자열에 포함된 퍼센트 인코딩은 허용하며, 정적 라우트의 Query 인코딩도 기존처럼 거부하지 않는다.
- 기존 `/`, `/hello`, `/echo`, `/cafe`, `/menu`, `/assets/...` 동작과 회귀 계약을 유지한다.

---

## 3. 서버 내부 동적 응답 (`GET /greet`) 요구사항

### 입력(Query 파라미터) 규칙

- Query 문자열은 `&`로 쌍을 나누고 빈 세그먼트(`&&`, 앞·뒤 `&`)는 무시한다. 각 쌍은 첫 번째 `=`로 key와 value를 나누며(`=`가 없으면 value는 빈 문자열 `""`), 분리 후 key와 value 모두에 대해 `+` → 공백(`' '`) 치환 및 `%XX` 퍼센트 디코딩을 적용한다.
- 불완전한 `%` 구문이나 디코딩 결과 `%00`(NUL 바이트)이 Query 내 **어느 파라미터의 key 또는 value에든** 포함되면 `400 Bad Request`로 거부한다.
- key는 대소문자를 구별해 `"name"`만 인식하며, `"name"`이 여러 번 등장하면 **첫 번째 매칭 값**을 사용한다 (`/greet?name&name=Alice`는 첫 번째 빈 값 선택).
- 추출된 `name` 값에 제어 문자(`0x01`~`0x1F`, `0x7F`)가 하나라도 포함되면 `400 Bad Request`로 거부한다.
- 추출된 `name` 값의 앞뒤 ASCII 공백(`' '`)은 제거하며, `name`이 없거나 빈 값 또는 공백만 있는 경우 기본 호칭 **`"손님"`**으로 응답한다.
- `name` 길이 별도 상한은 두지 않으며 요청 헤더 제한(`REQ_HEADER_MAX_BYTES`) 이내 입력을 모두 수용한다.
- `GET /greet`의 요청 본문은 무시한다.

### 출력 및 보안 규칙

- 정상 응답 상태는 `200 OK`, `Content-Type`은 `text/html; charset=utf-8`이며 `Content-Length`를 정확히 명시한다.
- 사용자 입력을 HTML에 삽입할 때 `&`, `<`, `>`, `"`, `'` 5문자는 각각 `&amp;`, `&lt;`, `&gt;`, `&quot;`, `&#39;`로 이스케이프한다(XSS 방어).
- 유효한 UTF-8 바이트(한글 등)와 유효하지 않은 UTF-8 바이트 모두 5개 HTML 특수문자를 제외하고 바이트 단위로 손실 없이 출력한다.
- 핸들러 내부의 `400`/`500` 오류는 기존 공통 오류 형식(`text/plain`)을 유지한다.

---

## 4. 외부 CGI 실행 (`GET /cgi-bin/...`) 요구사항

### 시작 조건 및 실행 경로 보안

- 서버 시작 시 정적 루트(`public`) 검증 정책만 유지하며, `cgi-bin` 디렉터리가 없어도 서버는 정상 시작한다(`cgi-bin` 부재 시 CGI 요청만 `404`).
- `/cgi-bin/` 아래 단일 depth 일반 실행 파일만 실행한다. 빈 파일명(`/cgi-bin/`), 하위 경로·`PATH_INFO`(`/cgi-bin/a/b`), `..` 탈출, `cgi-bin` 밖 심볼릭 링크, 형제 디렉터리, 부재 파일, 디렉터리·FIFO 등 비일반 파일, 실행 권한이 없는 파일은 모두 개인 정책 `404`로 거부한다.

### 프로토콜, 환경 및 오류 정책

- `GET` 요청에 대해 `REQUEST_METHOD=GET`, `QUERY_STRING`(원본 Query), `SCRIPT_NAME`, `SERVER_PROTOCOL=HTTP/1.1` 등 최소 환경변수만 전달해 격리 실행한다. 서버의 소켓 파일 디스크립터는 자식 프로세스로 상속되지 않아야 한다.
- CGI 출력은 `Content-Type` 헤더(필수)와 빈 줄(`\r\n\r\n` 또는 `\n\n`) 뒤에 본문이 이어지는 형식을 따른다. 선택적 `Status:` 헤더(200~599 중 `204`·`304` 제외)를 허용하며, 미지정 시 `200`으로 처리한다. CGI 출력의 `Content-Length`·`Transfer-Encoding`·`Connection` 등 프레이밍·연결 헤더는 클라이언트 응답에 전달하지 않는다.
- 실패 상태 코드 구분:
  - 서버 내부 자원 실패(메모리 할당, 파이프·프로세스 생성 실패 등) → **`500 Internal Server Error`**
  - CGI 프로그램 측 실패(실행 실패, 빈 출력, 헤더 형식·`Content-Type`·`Status:` 위반, 헤더 단계 응답 지연 초과, 헤더 전 비정상 종료) → **`502 Bad Gateway`**
- 응답 지연·자원 회수: CGI 출력 대기에는 5초 제한을 두며, 정상 종료·오류·타임아웃·클라이언트 조기 종료 모든 경우에 자식 프로세스를 회수해 좀비 프로세스와 열린 파일 디스크립터·메모리 누수가 남지 않아야 한다.

---

## 5. 청크 전송 (`Transfer-Encoding: chunked`) 요구사항

- 사전에 전체 본문 길이를 알 수 없는 CGI 스트림 응답은 `Content-Length` 대신 `Transfer-Encoding: chunked`와 `Connection: close` 헤더로 전송한다.
- 각 본문 조각은 `<16진수 길이>\r\n<데이터>\r\n` 형식으로 전송한다.
- CGI 프로그램이 정상 종료(EOF 및 종료 코드 `0`)한 경우에만 종료 청크 `0\r\n\r\n`을 전송해 응답 완료를 알린다.
- 스트리밍 도중 CGI가 비정상 종료하거나 타임아웃·읽기·송신 오류가 발생하면 **`0\r\n\r\n`을 전송하지 않고** 즉시 전송을 중단해 클라이언트가 불완전한 응답을 판별할 수 있게 한다.
- 기존 고정 길이 응답(`/`, `/hello`, `/echo`, `/cafe`, `/menu`, `/assets/...`, `/greet`, 오류 응답)의 `Content-Length` 전송 방식은 그대로 유지한다.

---

## 참고

- 내부 아키텍처·책임 분리·함수 흐름 설계 결정은 [iteration-3-dynamic-design.md](iteration-3-dynamic-design.md)에 정의한다.
- 테스트 기준은 [iteration-3-dynamic-tests.md](iteration-3-dynamic-tests.md)에 정의한다.
- 정적 서빙과 마찬가지로 이 문서는 개인 브랜치 기준이며, 팀 Issue #3의 미결정 차이(정적 접근 불가 403 vs 404, CLI 루트 지정 등)와는 별도로 관리한다.
