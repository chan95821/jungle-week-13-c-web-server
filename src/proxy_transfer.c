#define _POSIX_C_SOURCE 200809L
#include "proxy_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

void proxy_prepare_response(const struct sockaddr_in *backend,const struct http_request *request,
    const volatile sig_atomic_t *running,struct router_response *response) {
    proxy_prepare_tracked_response(backend,request,running,response,NULL);
}
/* Prepare locally, then transfer backend/buffer ownership to the provider. */
void proxy_prepare_tracked_response(const struct sockaddr_in *backend,
                            const struct http_request *request,
                            const volatile sig_atomic_t *running,
                            struct router_response *response,struct distribution_timing *timing) {
    struct proxy_stream *stream = calloc(1, sizeof(*stream));
    if (!stream) { router_prepare_error(response, 500, NULL); return; }
    stream->backend_fd = -1;
    stream->running = running;
    stream->selection_timing=timing;
    if(timing)stream->selection_clock_valid=clock_gettime(CLOCK_MONOTONIC,&stream->selection_started)==0;
    if (connect_backend(stream, backend) < 0 ||
        forward_request_to_backend(stream, backend, request) < 0 ||
        make_deadline(&stream->header_deadline, PROXY_HEADER_SECONDS) < 0 ||
        prepare_backend_response_headers(stream) < 0 ||
        make_deadline(&stream->body_deadline, PROXY_BODY_SECONDS) < 0) {
        int saved_errno = errno;
        finish_proxy_timing(stream,saved_errno==ECANCELED?DISTRIBUTION_CANCELLED:DISTRIBUTION_FAILED);
        cleanup_backend_response(stream);
        int status = saved_errno == ENOMEM ? 500 :
                     saved_errno == ETIMEDOUT ? 504 : 502;
        router_prepare_error(response, status, NULL);
        return;
    }
    stream->body_started = true;
    stream->remaining = stream->content_length;
    if(stream->framing==BACKEND_NO_BODY || (stream->framing==BACKEND_LENGTH && !stream->remaining))
        finish_proxy_timing(stream,DISTRIBUTION_COMPLETED);
    *response = (struct router_response){
        .status = stream->status,
        .length = stream->framing == BACKEND_LENGTH ? stream->content_length : 0,
        .chunked = stream->framing == BACKEND_CHUNKED || stream->framing == BACKEND_CLOSE,
        .wire_header = {stream->client_header, stream->client_header_length},
        .body_provider = {
            .next_func = read_backend_body_chunk,
            .cleanup_func = cleanup_backend_response,
            .state = stream,
            .trailers = &stream->trailers
        }
    };
}

void finish_proxy_timing(struct proxy_stream *stream,enum distribution_sample_result result) {
    int saved=errno;
    if(stream->selection_timing && stream->selection_clock_valid && !stream->selection_finished) {
        struct timespec now;struct distribution_observation observation;
        if(clock_gettime(CLOCK_MONOTONIC,&now)==0 &&
           distribution_elapsed_us(&stream->selection_started,&now,&observation,result)==0)
            update_distribution_timing(stream->selection_timing,&observation);
    }
    stream->selection_finished=true;errno=saved;
}
void cleanup_backend_response(void *state) {
    struct proxy_stream *stream = state;
    if (stream->backend_fd >= 0) close(stream->backend_fd);
    free(stream->client_header);
    free(stream->connection_fields);
    free(stream->trailer_buffer);
    free(stream);
}
