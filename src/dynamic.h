#ifndef DYNAMIC_H
#define DYNAMIC_H

#include "router.h"

/* 64비트 정수 하나를 JSON으로 표현하는 두 API에 충분한 고정 공간이다.
 * 입력 크기 상한이나 파일 전송 버퍼와는 다른 용도다. handle_client가 소유한다. */
#define DYNAMIC_RESPONSE_CAPACITY 256

/* 본문을 기다리기 전에 판단할 수 있는 API 전용 헤더 조건을 확인한다.
 * 성공 0, 잘못된/지원하지 않는 조건은 HTTP 상태 코드. 소켓 I/O는 하지 않는다. */
int dynamic_check_headers(const struct http_request *request, enum http_route route);

/* 본문 수신이 완료된 뒤 호출한다. 입력 해석 → 계산 → JSON 생성을 수행한다.
 * buffer는 호출자 소유이며 전송 완료까지 유지해야 한다. response.body는 이 공간을
 * 빌려 가리킨다. 함수 내부 지역 배열의 주소를 반환하거나 메모리를 할당하지 않는다.
 * 성공 0, 실패 HTTP 상태 코드. 전송과 자원 해제는 호출자 책임이다. */
int prepare_dynamic_response(const struct http_request *request, enum http_route route,
                              char *buffer, size_t capacity, struct router_response *response);

#endif
