#ifndef CGI_PROTOCOL_H
#define CGI_PROTOCOL_H
#include "../router.h"
enum { CGI_HEADER_MAX_BYTES=8192, CGI_READ_TIMEOUT_MS=5000, CGI_CHUNK_BUFFER_SIZE=16384, CGI_STOP_GRACE_MS=200 };
size_t cgi_find_header_end(const unsigned char *bytes,size_t length,size_t *separator);
bool cgi_parse_headers(const unsigned char *bytes,size_t length,char **content_type,int *status);
int cgi_resolve_script(struct http_slice path,char **resolved,char **script_name);
char *cgi_make_env_var(const char *key,struct http_slice value);
/* Prepared only by the file helper, stable until its job cleanup completes. */
struct cgi_launch { char *resolved,*script_name;char *env[6];int input_fd; };
void prepare_cgi_launch(const struct http_request *request,struct router_response *response);
#endif
