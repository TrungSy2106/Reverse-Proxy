#ifndef PROXY_HANDLER_H
#define PROXY_HANDLER_H

#include <winsock2.h>

// Initialize connection pool
void proxy_handler_init(void);

// Cleanup connection pool
void proxy_handler_cleanup(void);

// Fast forwarding with connection pooling
unsigned char *proxy_handler_forward_fast(const char *host, int port, const char *request, int request_len, int *out_len);

// Legacy function for compatibility
unsigned char *proxy_handler_forward(const char *host, int port, const char *request, int request_len, int *out_len);

#endif