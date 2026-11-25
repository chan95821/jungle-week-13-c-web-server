#ifndef EVENT_SERVER_H
#define EVENT_SERVER_H

#include "proxy.h"

enum { EVENT_DEFAULT_WORKERS = 4, EVENT_MAX_WORKERS = 64 };

/* Linux/WSL2 B 모델의 진입점. listener/config의 수명은 호출자가 소유한다. */
int run_event_server(int listener_fd, const struct proxy_config *config,
                     unsigned int worker_count);

#endif
