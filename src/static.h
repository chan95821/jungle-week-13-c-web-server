#ifndef STATIC_H
#define STATIC_H

#include <stdint.h>
#include "http.h"

/* Open once at startup. The configured root may itself resolve through links. */
int static_open_root(const char *path);

/* Each successful call transfers one regular-file FD to the caller. */
int static_open_file(int rootfd, struct http_slice path, int *filefd,
                     uintmax_t *length, const char **content_type);

/* Decode only the static path, not query data. Return 0 or an HTTP status. */
int static_decode_path(struct http_slice path, char *decoded, size_t capacity);
const char *static_mime_type(const char *path);

#endif