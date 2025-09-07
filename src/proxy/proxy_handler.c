#include "proxy_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_POOL_SIZE 20
#define KEEP_ALIVE_TIMEOUT 30000 // 30 seconds

// Connection pool structure
typedef struct {
    SOCKET sock;
    char host[256];
    int port;
    DWORD last_used;
    int in_use;
} pool_connection_t;

static pool_connection_t connection_pool[MAX_POOL_SIZE];
static CRITICAL_SECTION pool_mutex;
static int pool_initialized = 0;

void proxy_handler_init() {
    if (!pool_initialized) {
        InitializeCriticalSection(&pool_mutex);
        memset(connection_pool, 0, sizeof(connection_pool));
        for (int i = 0; i < MAX_POOL_SIZE; i++) {
            connection_pool[i].sock = INVALID_SOCKET;
        }
        pool_initialized = 1;
        printf("🔗 Connection pool initialized with %d slots\n", MAX_POOL_SIZE);
    }
}

void proxy_handler_cleanup() {
    if (pool_initialized) {
        EnterCriticalSection(&pool_mutex);
        for (int i = 0; i < MAX_POOL_SIZE; i++) {
            if (connection_pool[i].sock != INVALID_SOCKET) {
                closesocket(connection_pool[i].sock);
                connection_pool[i].sock = INVALID_SOCKET;
            }
        }
        LeaveCriticalSection(&pool_mutex);
        DeleteCriticalSection(&pool_mutex);
        pool_initialized = 0;
    }
}

// Get connection from pool or create new one
SOCKET get_pooled_connection(const char *host, int port) {
    if (!pool_initialized) return INVALID_SOCKET;
    
    EnterCriticalSection(&pool_mutex);
    
    DWORD now = GetTickCount();
    
    // First, clean up expired connections
    for (int i = 0; i < MAX_POOL_SIZE; i++) {
        if (connection_pool[i].sock != INVALID_SOCKET && 
            !connection_pool[i].in_use &&
            (now - connection_pool[i].last_used) > KEEP_ALIVE_TIMEOUT) {
            closesocket(connection_pool[i].sock);
            connection_pool[i].sock = INVALID_SOCKET;
        }
    }
    
    // Look for existing connection to same host:port
    for (int i = 0; i < MAX_POOL_SIZE; i++) {
        if (connection_pool[i].sock != INVALID_SOCKET &&
            !connection_pool[i].in_use &&
            strcmp(connection_pool[i].host, host) == 0 &&
            connection_pool[i].port == port) {
            
            // Test if connection is still alive with a quick check
            fd_set write_fds;
            struct timeval timeout = {0, 0};
            FD_ZERO(&write_fds);
            FD_SET(connection_pool[i].sock, &write_fds);
            
            if (select(0, NULL, &write_fds, NULL, &timeout) >= 0) {
                connection_pool[i].in_use = 1;
                connection_pool[i].last_used = now;
                SOCKET sock = connection_pool[i].sock;
                LeaveCriticalSection(&pool_mutex);
                return sock;
            } else {
                // Connection is dead, close it
                closesocket(connection_pool[i].sock);
                connection_pool[i].sock = INVALID_SOCKET;
            }
        }
    }
    
    LeaveCriticalSection(&pool_mutex);
    return INVALID_SOCKET;
}

// Return connection to pool
void return_pooled_connection(SOCKET sock, const char *host, int port, int keep_alive) {
    if (!pool_initialized || sock == INVALID_SOCKET) return;
    
    EnterCriticalSection(&pool_mutex);
    
    // Find the connection in pool
    for (int i = 0; i < MAX_POOL_SIZE; i++) {
        if (connection_pool[i].sock == sock) {
            connection_pool[i].in_use = 0;
            connection_pool[i].last_used = GetTickCount();
            
            if (!keep_alive) {
                closesocket(connection_pool[i].sock);
                connection_pool[i].sock = INVALID_SOCKET;
            }
            LeaveCriticalSection(&pool_mutex);
            return;
        }
    }
    
    // If not in pool and we want to keep it, add it
    if (keep_alive) {
        for (int i = 0; i < MAX_POOL_SIZE; i++) {
            if (connection_pool[i].sock == INVALID_SOCKET) {
                connection_pool[i].sock = sock;
                strcpy(connection_pool[i].host, host);
                connection_pool[i].port = port;
                connection_pool[i].last_used = GetTickCount();
                connection_pool[i].in_use = 0;
                LeaveCriticalSection(&pool_mutex);
                return;
            }
        }
    }
    
    // Pool is full or we don't want to keep it
    closesocket(sock);
    LeaveCriticalSection(&pool_mutex);
}

// Fast version with connection pooling
unsigned char *proxy_handler_forward_fast(const char *host, int port, const char *request, int request_len, int *out_len) {
    SOCKET sock;
    struct addrinfo hints, *res;
    char buffer[8192];
    unsigned char *response = NULL;
    int use_keep_alive = 0;
    
    *out_len = 0;
    
    // Try to get from connection pool first
    sock = get_pooled_connection(host, port);
    
    if (sock == INVALID_SOCKET) {
        // Create new connection
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        char port_str[16];
        sprintf(port_str, "%d", port);
        
        if (getaddrinfo(host, port_str, &hints, &res) != 0) {
            return NULL;
        }
        
        sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock == INVALID_SOCKET) {
            freeaddrinfo(res);
            return NULL;
        }
        
        // Set socket options for performance
        int opt = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt)); // Disable Nagle
        
        int bufsize = 32768; // 32KB buffer
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&bufsize, sizeof(bufsize));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&bufsize, sizeof(bufsize));
        
        // Quick connect timeout
        int timeout = 3000; // 3 seconds
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
        
        if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
            closesocket(sock);
            freeaddrinfo(res);
            return NULL;
        }
        
        freeaddrinfo(res);
        use_keep_alive = 1; // New connection, can be reused
    }
    
    // Modify request to add Connection: keep-alive if not present
    char *modified_request = NULL;
    int modified_len = request_len;
    
    if (!strstr(request, "Connection:")) {
        // Add Connection: keep-alive
        char *header_end = strstr(request, "\r\n\r\n");
        if (header_end) {
            int headers_len = header_end - request;
            int body_len = request_len - headers_len - 4;
            
            modified_len = request_len + 24; // "Connection: keep-alive\r\n"
            modified_request = malloc(modified_len + 1);
            
            memcpy(modified_request, request, headers_len);
            memcpy(modified_request + headers_len, "\r\nConnection: keep-alive\r\n\r\n", 28);
            if (body_len > 0) {
                memcpy(modified_request + headers_len + 28, header_end + 4, body_len);
            }
            modified_len = headers_len + 28 + body_len;
        } else {
            modified_request = (char*)request;
            use_keep_alive = 0;
        }
    } else {
        modified_request = (char*)request;
        // Check if it already has keep-alive
        use_keep_alive = (strstr(request, "Connection: keep-alive") != NULL);
    }
    
    // Send request
    int sent = 0;
    while (sent < modified_len) {
        int bytes_sent = send(sock, modified_request + sent, modified_len - sent, 0);
        if (bytes_sent == SOCKET_ERROR) {
            if (modified_request != request) free(modified_request);
            return_pooled_connection(sock, host, port, 0);
            return NULL;
        }
        sent += bytes_sent;
    }
    
    if (modified_request != request) free(modified_request);
    
    // Read response with better buffering
    response = malloc(8192);
    int capacity = 8192;
    
    while (1) {
        int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        
        // Expand buffer if needed
        if (*out_len + n > capacity) {
            capacity *= 2;
            unsigned char *new_response = realloc(response, capacity);
            if (!new_response) {
                free(response);
                return_pooled_connection(sock, host, port, 0);
                return NULL;
            }
            response = new_response;
        }
        
        memcpy(response + *out_len, buffer, n);
        *out_len += n;
        
        // Quick check for complete response (optimize for small responses)
        if (*out_len >= 4 && strstr((char*)response, "\r\n\r\n")) {
            char *header_end = strstr((char*)response, "\r\n\r\n");
            char *content_length_str = strstr((char*)response, "Content-Length:");
            
            if (content_length_str && content_length_str < header_end) {
                int content_length = 0;
                sscanf(content_length_str, "Content-Length: %d", &content_length);
                
                int headers_len = header_end - (char*)response + 4;
                int body_received = *out_len - headers_len;
                
                if (body_received >= content_length) {
                    break; // Complete response
                }
            } else if (strstr((char*)response, "Transfer-Encoding: chunked")) {
                if (strstr((char*)response, "\r\n0\r\n\r\n")) {
                    break; // End of chunked response
                }
            }
        }
    }
    
    // Check if backend wants to keep connection alive
    int backend_keep_alive = use_keep_alive && 
                           strstr((char*)response, "Connection: keep-alive") != NULL;
    
    return_pooled_connection(sock, host, port, backend_keep_alive);
    
    return response;
}

// Legacy function for compatibility
unsigned char *proxy_handler_forward(const char *host, int port, const char *request, int request_len, int *out_len) {
    return proxy_handler_forward_fast(host, port, request, request_len, out_len);
}