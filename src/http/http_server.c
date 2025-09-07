#include "http_server.h"
#include "../proxy/proxy_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

// Global backend info
static char g_backend_host[256];
static int g_backend_port;

// Extract client IP from socket
void get_client_ip(SOCKET client_fd, char* ip_buffer, int buffer_size) {
    struct sockaddr_in client_addr;
    int addr_len = sizeof(client_addr);
    
    if (getpeername(client_fd, (struct sockaddr*)&client_addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buffer, buffer_size);
    } else {
        strcpy(ip_buffer, "127.0.0.1");
    }
}

// Fix request headers for backend
char* fix_request_headers(const char* original_request, int request_len, const char* client_ip, int* new_len) {
    // Find header end
    char* header_end = strstr(original_request, "\r\n\r\n");
    if (!header_end) return NULL;
    
    int headers_len = header_end - original_request;
    int body_len = request_len - headers_len - 4;
    
    // Parse request line
    char method[16], path[2048], version[16];
    if (sscanf(original_request, "%15s %2047s %15s", method, path, version) != 3) {
        return NULL;
    }
    
    // Build new headers
    char new_headers[8192];
    int pos = 0;
    
    // Add request line
    pos += sprintf(new_headers + pos, "%s %s %s\r\n", method, path, version);
    
    // Add essential proxy headers first
    pos += sprintf(new_headers + pos, "Host: %s:%d\r\n", g_backend_host, g_backend_port);
    pos += sprintf(new_headers + pos, "X-Forwarded-For: %s\r\n", client_ip);
    pos += sprintf(new_headers + pos, "X-Real-IP: %s\r\n", client_ip);
    pos += sprintf(new_headers + pos, "X-Forwarded-Proto: http\r\n");
    pos += sprintf(new_headers + pos, "Via: 1.1 reverse-proxy\r\n");
    
    // Parse and copy other headers (skip problematic ones)
    const char* line = original_request;
    line = strstr(line, "\r\n") + 2; // Skip request line
    
    while (line < header_end) {
        const char* line_end = strstr(line, "\r\n");
        if (!line_end) break;
        
        // Skip headers that proxy should handle
        if (_strnicmp(line, "Host:", 5) == 0 ||
            _strnicmp(line, "Connection:", 11) == 0 ||
            _strnicmp(line, "Proxy-Connection:", 17) == 0 ||
            _strnicmp(line, "Keep-Alive:", 11) == 0 ||
            _strnicmp(line, "Upgrade:", 8) == 0 ||
            _strnicmp(line, "X-Forwarded-For:", 16) == 0 ||
            _strnicmp(line, "X-Real-IP:", 10) == 0 ||
            _strnicmp(line, "Via:", 4) == 0) {
            // Skip these headers
            line = line_end + 2;
            continue;
        }
        
        // Copy other headers
        int header_len = line_end - line;
        if (pos + header_len + 2 < sizeof(new_headers)) {
            memcpy(new_headers + pos, line, header_len);
            pos += header_len;
            memcpy(new_headers + pos, "\r\n", 2);
            pos += 2;
        }
        
        line = line_end + 2;
    }
    
    // Add connection management
    pos += sprintf(new_headers + pos, "Connection: keep-alive\r\n");
    
    // End headers
    pos += sprintf(new_headers + pos, "\r\n");
    
    // Allocate new request
    *new_len = pos + body_len;
    char* new_request = malloc(*new_len + 1);
    if (!new_request) return NULL;
    
    // Copy headers and body
    memcpy(new_request, new_headers, pos);
    if (body_len > 0) {
        memcpy(new_request + pos, header_end + 4, body_len);
    }
    new_request[*new_len] = '\0';
    
    printf("   Fixed headers:\n");
    printf("   Original Host header → Host: %s:%d\n", g_backend_host, g_backend_port);
    printf("   Added X-Forwarded-For: %s\n", client_ip);
    printf("   Request size: %d → %d bytes\n", request_len, *new_len);
    
    return new_request;
}

// Fix response headers for client
char* fix_response_headers(const char* original_response, int response_len, int* new_len) {
    // Find header end
    char* header_end = strstr(original_response, "\r\n\r\n");
    if (!header_end) {
        // No headers found, return as-is
        *new_len = response_len;
        char* copy = malloc(response_len);
        if (copy) memcpy(copy, original_response, response_len);
        return copy;
    }
    
    int headers_len = header_end - original_response;
    int body_len = response_len - headers_len - 4;
    
    // Parse status line
    char status_line[256];
    const char* first_line_end = strstr(original_response, "\r\n");
    if (!first_line_end || first_line_end > header_end) return NULL;
    
    int status_len = first_line_end - original_response;
    if (status_len >= sizeof(status_line)) status_len = sizeof(status_line) - 1;
    memcpy(status_line, original_response, status_len);
    status_line[status_len] = '\0';
    
    // Build new headers
    char new_headers[8192];
    int pos = 0;
    
    // Add status line
    pos += sprintf(new_headers + pos, "%s\r\n", status_line);
    
    // Parse and filter response headers
    const char* line = first_line_end + 2;
    while (line < header_end) {
        const char* line_end = strstr(line, "\r\n");
        if (!line_end) break;
        
        // Skip hop-by-hop headers that proxy should not forward
        if (_strnicmp(line, "Connection:", 11) == 0 ||
            _strnicmp(line, "Keep-Alive:", 11) == 0 ||
            _strnicmp(line, "Proxy-Authenticate:", 19) == 0 ||
            _strnicmp(line, "Proxy-Authorization:", 20) == 0 ||
            _strnicmp(line, "TE:", 3) == 0 ||
            _strnicmp(line, "Trailers:", 9) == 0 ||
            _strnicmp(line, "Upgrade:", 8) == 0) {
            // Skip these headers
            line = line_end + 2;
            continue;
        }
        
        // Fix problematic headers
        if (_strnicmp(line, "Location:", 9) == 0) {
            // Fix redirect URLs that point to backend
            char location[1024];
            const char* value_start = line + 9;
            while (*value_start == ' ') value_start++;
            
            int value_len = line_end - value_start;
            if (value_len < sizeof(location)) {
                memcpy(location, value_start, value_len);
                location[value_len] = '\0';
                
                // Replace backend host with proxy host
                char backend_url[256];
                sprintf(backend_url, "http://%s:%d", g_backend_host, g_backend_port);
                
                if (strstr(location, backend_url)) {
                    pos += sprintf(new_headers + pos, "Location: http://localhost:8080%s\r\n", 
                                 location + strlen(backend_url));
                    line = line_end + 2;
                    continue;
                }
            }
        }
        
        // Copy other headers as-is
        int header_len = line_end - line;
        if (pos + header_len + 2 < sizeof(new_headers)) {
            memcpy(new_headers + pos, line, header_len);
            pos += header_len;
            memcpy(new_headers + pos, "\r\n", 2);
            pos += 2;
        }
        
        line = line_end + 2;
    }
    
    // Add proxy identification
    pos += sprintf(new_headers + pos, "Via: 1.1 reverse-proxy\r\n");
    pos += sprintf(new_headers + pos, "X-Proxy: Custom-Reverse-Proxy/1.0\r\n");
    
    // Manage connection based on client request
    pos += sprintf(new_headers + pos, "Connection: close\r\n");
    
    // End headers
    pos += sprintf(new_headers + pos, "\r\n");
    
    // Allocate new response
    *new_len = pos + body_len;
    char* new_response = malloc(*new_len + 1);
    if (!new_response) return NULL;
    
    // Copy headers and body
    memcpy(new_response, new_headers, pos);
    if (body_len > 0) {
        memcpy(new_response + pos, header_end + 4, body_len);
    }
    
    printf("📝 Fixed response headers:\n");
    printf("   Removed hop-by-hop headers\n");
    printf("   Added Via header\n");
    printf("   Response size: %d → %d bytes\n", response_len, *new_len);
    
    return new_response;
}

// Thread function to handle each client
unsigned __stdcall handle_client_thread(void* arg) {
    SOCKET client_fd = (SOCKET)(uintptr_t)arg;
    char client_ip[INET_ADDRSTRLEN];
    
    get_client_ip(client_fd, client_ip, sizeof(client_ip));
    printf("🔗 New client from %s\n", client_ip);
    
    // Read request with timeout
    char buffer[16384];
    int total_received = 0;
    
    // Set timeout for reading
    int timeout = 10000; // 10 seconds
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    
    // Read initial chunk
    int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        printf("❌ Failed to read from client %s\n", client_ip);
        closesocket(client_fd);
        return 0;
    }
    
    total_received = n;
    buffer[total_received] = '\0';
    
    // Check if we have complete headers
    char* header_end = strstr(buffer, "\r\n\r\n");
    if (!header_end) {
        // Try to read more for headers
        n = recv(client_fd, buffer + total_received, sizeof(buffer) - total_received - 1, 0);
        if (n > 0) {
            total_received += n;
            buffer[total_received] = '\0';
            header_end = strstr(buffer, "\r\n\r\n");
        }
    }
    
    if (!header_end) {
        printf("❌ Incomplete HTTP request from %s\n", client_ip);
        closesocket(client_fd);
        return 0;
    }
    
    // Check for POST/PUT body
    char* content_length_str = strstr(buffer, "Content-Length:");
    if (content_length_str && content_length_str < header_end) {
        int content_length = 0;
        sscanf(content_length_str, "Content-Length: %d", &content_length);
        
        if (content_length > 0) {
            int headers_len = header_end - buffer + 4;
            int body_received = total_received - headers_len;
            
            // Read remaining body if needed
            while (body_received < content_length && total_received < sizeof(buffer) - 1) {
                n = recv(client_fd, buffer + total_received, 
                        min(content_length - body_received, sizeof(buffer) - total_received - 1), 0);
                if (n <= 0) break;
                total_received += n;
                body_received += n;
                buffer[total_received] = '\0';
            }
        }
    }
    
    printf("📥 Received %d bytes from %s\n", total_received, client_ip);
    
    // Fix request headers
    int fixed_request_len;
    char* fixed_request = fix_request_headers(buffer, total_received, client_ip, &fixed_request_len);
    
    if (!fixed_request) {
        printf("❌ Failed to fix request headers from %s\n", client_ip);
        closesocket(client_fd);
        return 0;
    }
    
    // Forward to backend
    int resp_len;
    unsigned char* backend_response = proxy_handler_forward(g_backend_host, g_backend_port, 
                                                          fixed_request, fixed_request_len, &resp_len);
    
    free(fixed_request);
    
    if (backend_response && resp_len > 0) {
        // Fix response headers
        int fixed_response_len;
        char* fixed_response = fix_response_headers((char*)backend_response, resp_len, &fixed_response_len);
        
        if (fixed_response) {
            // Send fixed response to client
            int sent = 0;
            while (sent < fixed_response_len) {
                int bytes_sent = send(client_fd, fixed_response + sent, fixed_response_len - sent, 0);
                if (bytes_sent == SOCKET_ERROR) break;
                sent += bytes_sent;
            }
            printf("📤 Sent %d bytes to %s\n", sent, client_ip);
            free(fixed_response);
        } else {
            // Fallback: send original response
            send(client_fd, (char*)backend_response, resp_len, 0);
        }
        
        free(backend_response);
    } else {
        // Send proper error response
        const char* error_response = 
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 136\r\n"
            "Connection: close\r\n"
            "Via: 1.1 reverse-proxy\r\n"
            "\r\n"
            "<html><body><h1>502 Bad Gateway</h1><p>The backend server is not available.</p><p>Proxy: Custom-Reverse-Proxy</p></body></html>";
        
        send(client_fd, error_response, strlen(error_response), 0);
        printf("❌ Sent 502 error to %s\n", client_ip);
    }
    
    closesocket(client_fd);
    printf("🔌 Closed connection to %s\n", client_ip);
    return 0;
}

void start_http_server(int listen_port, const char *backend_host, int backend_port) {
    WSADATA wsa;
    SOCKET server_fd;
    struct sockaddr_in server_addr, client_addr;
    int addrlen = sizeof(client_addr);
    
    // Store backend info globally
    strcpy(g_backend_host, backend_host);
    g_backend_port = backend_port;
    
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("❌ WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("❌ Socket failed: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listen_port);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("❌ Bind failed: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        return;
    }
    
    if (listen(server_fd, 100) == SOCKET_ERROR) {
        printf("❌ Listen failed: %d\n", WSAGetLastError());
        closesocket(server_fd);
        WSACleanup();
        return;
    }
    
    printf("🚀 Threaded proxy with proper headers listening on port %d\n", listen_port);
    printf("📡 Forwarding to %s:%d with header fixes\n", backend_host, backend_port);
    printf("🔧 Features: X-Forwarded-For, proper Host header, hop-by-hop filtering\n");
    
    while (1) {
        SOCKET client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd == INVALID_SOCKET) {
            continue;
        }
        
        // Create thread to handle client
        uintptr_t thread_handle = _beginthreadex(NULL, 0, handle_client_thread, 
                                               (void*)(uintptr_t)client_fd, 0, NULL);
        if (thread_handle) {
            CloseHandle((HANDLE)thread_handle);
        } else {
            // Fallback to synchronous handling
            handle_client_thread((void*)(uintptr_t)client_fd);
        }
    }
    
    closesocket(server_fd);
    WSACleanup();
}