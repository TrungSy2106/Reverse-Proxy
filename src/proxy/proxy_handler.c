#include "proxy_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

unsigned char *proxy_handler_forward(const char *host, int port, const char *request, int *out_len) {
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in serv_addr;
    struct addrinfo hints, *res;
    char buffer[4096];
    unsigned char *response = NULL;
    int total = 0;

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return NULL;
    }

    // resolve backend host
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    sprintf(port_str, "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        printf("getaddrinfo failed\n");
        WSACleanup();
        return NULL;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        printf("socket failed\n");
        freeaddrinfo(res);
        WSACleanup();
        return NULL;
    }

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        printf("connect failed\n");
        closesocket(sock);
        freeaddrinfo(res);
        WSACleanup();
        return NULL;
    }

    freeaddrinfo(res);

    // Gửi request sang backend
    send(sock, request, (int)strlen(request), 0);

    // Đọc toàn bộ response (binary safe)
    response = malloc(1);
    *out_len = 0;

    while (1) {
        int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n <= 0) break;

        response = realloc(response, *out_len + n);
        memcpy(response + *out_len, buffer, n);
        *out_len += n;
    }

    closesocket(sock);
    WSACleanup();

    return response;
}
