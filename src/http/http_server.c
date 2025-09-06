#include "http_server.h"
#include "../proxy/proxy_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

void start_http_server(int listen_port, const char *backend_host, int backend_port) {
    WSADATA wsa;
    SOCKET server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    int addrlen = sizeof(client_addr);

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return;
    }

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("Socket failed\n");
        WSACleanup();
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listen_port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Bind failed\n");
        closesocket(server_fd);
        WSACleanup();
        return;
    }

    listen(server_fd, 10);
    printf("Proxy listening on port %d, forwarding to %s:%d\n", listen_port, backend_host, backend_port);

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd == INVALID_SOCKET) {
            printf("Accept failed\n");
            continue;
        }

        char buffer[8192];
        int n = recv(client_fd, buffer, sizeof(buffer)-1, 0);
        if (n <= 0) {
            closesocket(client_fd);
            continue;
        }
        buffer[n] = '\0';

        int resp_len;
        unsigned char *backend_response = proxy_handler_forward(backend_host, backend_port, buffer, &resp_len);

        if (backend_response) {
            send(client_fd, (const char*)backend_response, resp_len, 0);
            free(backend_response);
        }

        closesocket(client_fd);
    }

    closesocket(server_fd);
    WSACleanup();
}
