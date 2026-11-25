#ifndef PROCESS_HTTP_SERVER_H
#define PROCESS_HTTP_SERVER_H
#include "proxy.h"
#include "process_signal_wait.h"
/* Parent borrows the configured listener/signal context; owns workers, mapping,
 * FIFO and ledgers until waitpid-confirmed cleanup. No implicit policy defaults. */
int run_process_http_server(int listener,const struct proxy_config *config,struct process_signal_wait *signals);
#endif
