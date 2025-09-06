#include "http_response.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int http_response_send(int client_fd, const char *body) {
    char buffer[2048];
    int len = snprintf(buffer, sizeof(buffer),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Length: %zu\r\n"
                       "Content-Type: text/plain\r\n"
                       "\r\n"
                       "%s",
                       strlen(body), body);

    return write(client_fd, buffer, len);
}
