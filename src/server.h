#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>
#include <stdint.h>

#define REQ_HEADER_MAX_BYTES 8192

int parse_port(const char *arg, uint16_t *out_port);
int open_listenfd(uint16_t port);
int recv_until_header_end(int fd, char *buf, size_t cap, size_t *out_len);
int send_all(int fd, const void *buf, size_t len);
void handle_client(int connfd);

#endif /* SERVER_H */
