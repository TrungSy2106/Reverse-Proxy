#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

typedef struct {
    char method[8];
    char path[1024];
    char version[16];
} http_request_t;

int http_request_parse(const char *raw, http_request_t *req);

#endif
