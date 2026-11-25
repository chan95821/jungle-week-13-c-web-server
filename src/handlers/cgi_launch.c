#define _XOPEN_SOURCE 700
#include "handlers.h"
#include "cgi_protocol.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void cleanup_cgi_launch(void *state){
    struct cgi_launch *launch=state;if(!launch)return;
    if(launch->input_fd>=0)close(launch->input_fd);
    for(size_t i=0;i<5;++i)free(launch->env[i]);
    free(launch->resolved);free(launch->script_name);free(launch);
}
void prepare_cgi_launch(const struct http_request *request,struct router_response *response){
    struct cgi_launch *launch=calloc(1,sizeof(*launch));
    if(!launch){router_prepare_error(response,500,NULL);return;}launch->input_fd=-1;
    int status=cgi_resolve_script(request->path,&launch->resolved,&launch->script_name);
    if(status)goto fail;
    launch->env[0]=cgi_make_env_var("REQUEST_METHOD",(struct http_slice){(const unsigned char *)"GET",3});
    launch->env[1]=cgi_make_env_var("QUERY_STRING",request->query);
    launch->env[2]=cgi_make_env_var("SCRIPT_NAME",(struct http_slice){(const unsigned char *)launch->script_name,strlen(launch->script_name)});
    launch->env[3]=cgi_make_env_var("SERVER_PROTOCOL",(struct http_slice){(const unsigned char *)"HTTP/1.1",8});
    launch->env[4]=cgi_make_env_var("PATH",(struct http_slice){(const unsigned char *)"/usr/local/bin:/usr/bin:/bin",28});
    status=500;for(size_t i=0;i<5;++i)if(!launch->env[i])goto fail;
    launch->input_fd=open("/dev/null",O_RDONLY|O_CLOEXEC);if(launch->input_fd<0)goto fail;
    *response=(struct router_response){.status=200,.body_provider={.state=launch,.cleanup_func=cleanup_cgi_launch}};return;
fail:
    cleanup_cgi_launch(launch);router_prepare_error(response,status,NULL);
}
