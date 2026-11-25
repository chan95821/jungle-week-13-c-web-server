# P1-4 HTTP/1.0 백엔드 응답 수용 결과

날짜: 2026-11-25. 기준 f32e17d, Iteration4/Proxy-YYJ의 기존 미커밋 P0-2 보완에 이어 적용했다. main 머지·커밋·푸시는 수행하지 않았다.

## 승인된 변경과 실제 코드

이전에는 백엔드 응답도 HTTP/1.1만 받았으며 그때의 C7 502는 당시 계약에 맞았다. 이번 사용자 승인으로 백엔드 응답에만 HTTP/1.0을 추가한다. 클라이언트 요청 및 백엔드로 전송하는 요청은 HTTP/1.1이다.

src/proxy_transfer.c:
- parse_backend_headers: 상태 줄의 HTTP/1.0·HTTP/1.1을 수용한다. HTTP/1.0의 1xx/Transfer-Encoding, 잘못된 버전·길이·프레이밍은 기존 502 오류 경로로 거절한다.
- build_client_response_headers: HTTP/1.1 접두사 뒤에 원본 상태 줄의 상태 코드·상태 문구를 복사한다. 원본 수신 버퍼를 바꾸지 않는다.
- 고정 길이·EOF 본문은 기존 공급자와 전송기를 사용한다. EOF 응답은 클라이언트 측 chunked로 재프레이밍한다.

응답 상태 구조·버퍼·FD 소유권은 그대로다. Host 재작성, 설정 파일 필수, 임시 기한, Round-Robin, P0-2 조기 400, 기존 HTTP/1.1 청크 확장·트레일러·1xx·204/304 동작은 보존한다. GET CL: 0 합성, HTTP/1.1 1xx의 Connection: close 및 다른 P1/P2 제안은 이번에 변경하지 않았다.

HTTP/1.0의 Transfer-Encoding을 오류로 처리하는 근거: RFC 9112 §6.1 https://www.rfc-editor.org/rfc/rfc9112.html#section-6.1

## 컴파일과 검증 결과

| 구분 | 결과 |
| --- | --- |
| make clean | 성공 |
| make all | 서버·CGI 컴파일·링크 성공 |
| make test | 7개 단위 바이너리 통과, ASan/UBSan 오류 보고 없음 |
| make verify | 기존 회귀 포함 E2E 5개 통과 |
| Python 구문 확인 | verify_iter4.py 성공 |
| 최종 테스트 실패 / skip | 없음 / 없음 |
| 사용자 설정 예시 | clean·빌드 전후 SHA256 동일 |

새 단위 케이스:
- HTTP/1.0 Content-Length 5/0, EOF, 204, 304, 409 Legacy conflict
- 출력 HTTP/1.1, 입력 헤더 불변, 중복 필드·상태 문구·Via 보존
- HTTP/1.0 100/103·TE·CL+TE·중복/오버플로우 길이·204+CL 및 미지원 버전 거절

새 E2E 케이스:
- C 백엔드의 고정 길이 hello와 바이너리 EOF, HTTP/1.1 출력 및 CL/chunked 프레이밍
- 409 문구, 204 헤더 미합성, 304 Content-Length 메타데이터 보존
- HTTP/1.0 1xx/TE -> 502
- HTTP/1.0 고정 본문 조기 종료 -> 원래 200 헤더 1개와 불완전 본문, 새 오류 응답 없음
- 클라이언트 HTTP/1.0 요청은 여전히 505
- 기존 P0-2·제출/조회·정적/동적/CGI·기한·FD 회수·시그널 회귀

실제 응답 백엔드는 tests/backends/server.c이며 기존 C 파서·소켓 I/O를 사용한다. Python HTTP 백엔드를 추가하지 않았다. 일반 E2E 서버는 -O2이며 sanitizer 단위 검사와 구분한다.

로그:
- build/http10-unit.log
- build/http10-e2e.log

## 미검증

부하·성능/S1/S_MAX 유지 여부, 외부 교차 스위트 전체, 광범위한 퍼징·경합·모든 실패 조합, 실제 운영 ACL은 재측정/검증하지 않았다. 테스트는 임시 설정·파일·포트를 사용했고 실제 정적 루트·운영 권한·사용자 설정은 변경하지 않았다. 이번 결과를 완전한 HTTP/1.0·1.1 준수나 성능 보장으로 확대하지 않는다.
