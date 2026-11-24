#ifndef PROXY_CONFIG_H
#define PROXY_CONFIG_H

#include "proxy.h"

/* 설정 파일 없이 쓰는 내장 기본값(제출 8001, 조회 8002·8003)과, 설정 파일이 허용하는 최대 조회 서버 수. */
#define PROXY_LOOKUP_MAX_BACKENDS 8

/* 라우트별 백엔드 주소를 확정한다. path가 NULL이면 내장 기본값을 쓴다.
 * 파일 형식은 "submit <host>:<port>" 한 줄과 "lookup <host>:<port>" 한 줄 이상이며,
 * 빈 줄과 '#'로 시작하는 주석 줄은 무시한다. IPv6는 "[::1]:8002"처럼 대괄호로 쓴다.
 * 성공 0, 실패 -1. 실패 시 stderr에 파일·줄·이유를 남기고 기존 설정을 바꾸지 않는다. */
int proxy_config_init(const char *path);

/* 확정된 업스트림을 빌려 준다. proxy_config_init() 성공 이후에만 유효하며,
 * 반환 포인터와 내부 배열은 프로세스 수명 동안 유지되고 호출자가 해제하지 않는다. */
const struct proxy_upstream *proxy_config_submission(void);
const struct proxy_upstream *proxy_config_lookup(void);

#endif
