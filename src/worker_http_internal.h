#ifndef WORKER_HTTP_INTERNAL_H
#define WORKER_HTTP_INTERNAL_H
#include "worker_http_io.h"
#include "worker_fd_channel.h"
#include "backend_request_lifetime.h"
#include "proxy_internal.h"
#include "concurrency_limits.h"
#include "server.h"
#include "worker_file_io.h"
#include <sys/epoll.h>
#define WORKER_CHANNEL_EVENT UINT64_MAX
#define WORKER_FILE_EVENT (UINT64_MAX-1)
_Static_assert((size_t)WORKER_RELAY_BYTES<=(size_t)PROXY_STREAM_BYTES,"worker relay must fit its owned stream buffer");
_Static_assert(WORKER_RELAY_BYTES>=REQ_HEADER_MAX_BYTES+5,"relay must hold one validated final trailer block");
_Static_assert(WORKER_RELAY_LOW_BYTES<WORKER_RELAY_BYTES,"worker relay watermarks must be ordered");
enum worker_http_phase { WORKER_SLOT_FREE, WORKER_CLIENT_HEADER, WORKER_CLIENT_BODY,
    WORKER_INTERIM_WRITE, WORKER_LOCAL_READY, WORKER_BACKEND_SELECT,
    WORKER_BACKEND_CONNECT, WORKER_BACKEND_REQUEST, WORKER_BACKEND_HEADER,
    WORKER_BACKEND_BODY, WORKER_CLIENT_OUTPUT, WORKER_CLIENT_DRAIN, WORKER_CLIENT_CLOSED, WORKER_FILE_OUTPUT, WORKER_CGI_PREPARE, WORKER_CGI_HEADER, WORKER_CGI_BODY };
enum worker_chunk_phase { WORKER_CHUNK_SIZE, WORKER_CHUNK_DATA, WORKER_CHUNK_CR,
    WORKER_CHUNK_LF, WORKER_CHUNK_TRAILERS };
struct worker_cgi_state;
struct worker_http_connection {
    uint32_t slot_generation;
    enum worker_http_phase phase;
    enum distribution_sample_result completion_result;
    int client_fd, backend_fd;
    uint32_t client_interest, backend_interest;
    bool final_started, client_half_closed, backend_finished, input_paused, terminator_pending;
    bool client_write_waiting, head_request, internal_failure;
    uint64_t connection_id, admitted_us;
    uint64_t header_due, body_due, input_idle_due, output_idle_due;
    uint64_t backend_due, backend_idle_due, paused_idle_ms, select_due, retry_at, drain_due;
    size_t header_used, header_end, body_used, request_sent, drain_bytes;
    unsigned char request_header[REQ_HEADER_MAX_BYTES];
    unsigned char *request_body;
    struct http_request request;
    struct request_route route;
    struct router_response local_response;
    struct worker_file_job *file_job;
    struct worker_cgi_state *cgi;
    size_t file_remaining;
    bool file_waiting, file_cancelled;
    const char *allow;
    unsigned char local_header[512];
    size_t local_header_length, local_header_sent, memory_sent;
    const unsigned char *memory_body;
    size_t memory_length;
    const struct sockaddr_in *backend_address;
    struct proxy_stream *response;
    struct proxy_response_headers *headers;
    size_t backend_request_length, response_header_sent;
    size_t relay_offset, relay_length, chunk_line_used, trailer_used;
    enum worker_chunk_phase chunk_phase;
    struct backend_request_lifetime lifetime;
};
struct cgi_launch;
int worker_cgi_start(struct worker_http_reactor *reactor,size_t slot,const struct cgi_launch *launch,uint64_t now);
void worker_cgi_advance(struct worker_http_reactor *reactor,size_t slot,uint64_t now);
int worker_cgi_update_interest(struct worker_http_reactor *reactor,size_t slot);
void worker_cgi_service(struct worker_http_reactor *reactor,uint64_t now);
void worker_cgi_cancel(struct worker_http_reactor *reactor,size_t slot,uint64_t now);
void worker_cgi_output_drained(struct worker_http_reactor *reactor,size_t slot,uint64_t now);
uint64_t worker_cgi_deadline(const struct worker_http_connection *connection);
void destroy_worker_cgi_children(struct worker_http_reactor *reactor);
int worker_file_start_response(struct worker_http_reactor *reactor,size_t slot,uint64_t now);
int worker_file_service_completions(struct worker_http_reactor *reactor,uint64_t now);
void worker_file_output_drained(struct worker_http_reactor *reactor,size_t slot,uint64_t now);
int worker_http_prepare_file_headers(struct worker_http_reactor *reactor,size_t slot,const struct router_response *response,uint64_t now);
struct worker_io_budget { size_t bytes; unsigned int calls; };
int worker_http_now(struct timespec *time,uint64_t *milliseconds);
int worker_http_update_interest(struct worker_http_reactor *reactor,size_t slot);
void worker_http_close_client(struct worker_http_reactor *reactor,size_t slot,
    uint64_t now,enum backend_request_outcome outcome);
void worker_http_backend_finished(struct worker_http_reactor *reactor,size_t slot,
    uint64_t now,enum backend_request_outcome outcome);
int worker_http_error_response(struct worker_http_reactor *reactor,size_t slot,int status,uint64_t now);
void worker_proxy_advance(struct worker_http_reactor *reactor,size_t slot,
    uint32_t events,uint64_t now,struct worker_io_budget *budget);
void worker_proxy_timer(struct worker_http_reactor *reactor,size_t slot,uint64_t now);
size_t worker_http_relay_used(const struct worker_http_connection *connection);
void worker_http_resume_backend_input(struct worker_http_connection *connection,uint64_t now);
void worker_http_service_deadlines(struct worker_http_reactor *reactor,uint64_t now);
int worker_http_publish_completed(struct worker_http_reactor *reactor,size_t slot,uint64_t now);
#endif
