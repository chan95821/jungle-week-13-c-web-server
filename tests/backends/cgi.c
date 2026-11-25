#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main(void){
    const char *query=getenv("QUERY_STRING");if(!query)query="";
    if(!strcmp(query,"slow") || !strcmp(query,"cancel")){
        const char *marker=!strcmp(query,"slow")?"cgi-slow.pid":"cgi-cancel.pid";
        FILE *file=fopen(marker,"w");assert(file);fprintf(file,"%ld\n",(long)getpid());assert(fclose(file)==0);
        while(access("cgi-release",F_OK)<0){struct timespec delay={0,10000000};nanosleep(&delay,NULL);}
    }
    if(!strcmp(query,"bad")){puts("bad header\n\n");return 0;}
    if(!strcmp(query,"binary")){
        fputs("Status: 201 Created\r\nContent-Type: application/octet-stream\r\n\r\n",stdout);
        const unsigned char body[]={'a',0,'b'};assert(fwrite(body,1,sizeof(body),stdout)==sizeof(body));return 0;
    }
    fputs("Content-Type: text/plain\r\n\r\n",stdout);fputs(*query?query:"cgi",stdout);
    return !strcmp(query,"fail")?1:0;
}
