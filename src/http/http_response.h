#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

int http_response_send(int client_fd, const char *body);

#endif
