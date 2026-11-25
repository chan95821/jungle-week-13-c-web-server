#ifndef EVENT_SERVER_H
#define EVENT_SERVER_H

#include "proxy.h"

/* 네트워크 작업자 수다. 기본 전체 스레드는 메인 1 + 네트워크 4 + 파일/CGI 보조 2 = 7개다. */
enum { EVENT_DEFAULT_WORKERS = 4, EVENT_MAX_WORKERS = 64 };

/* Linux/WSL2 B 모델의 진입점. main이 호출하고, 작업자 join 완료 후 main으로 돌아온다.
 * listener/config의 수명은 호출자가 소유하며, 이 함수는 연결별 FD를 직접 소유하지 않는다. */
int run_event_server(int listener_fd, const struct proxy_config *config,
                     unsigned int worker_count);

#endif
