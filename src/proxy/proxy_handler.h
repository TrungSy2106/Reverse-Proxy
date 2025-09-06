#ifndef PROXY_HANDLER_H
#define PROXY_HANDLER_H

unsigned char *proxy_handler_forward(const char *host, int port, const char *request, int *out_len);

#endif
