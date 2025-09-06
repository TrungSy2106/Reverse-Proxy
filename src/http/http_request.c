#include "http_request.h"
#include <stdio.h>
#include <string.h>

int http_request_parse(const char *raw, http_request_t *req) {
    if (sscanf(raw, "%7s %1023s %15s", req->method, req->path, req->version) != 3) {
        return -1;
    }
    return 0;
}
