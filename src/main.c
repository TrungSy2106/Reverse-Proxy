#include "http/http_server.h"
#include <stdio.h>

int main() {
    // 🔹 Proxy listen ở cổng 8080
    int listen_port = 8080;

    // 🔹 Backend server chạy ở localhost:5000 (Live Server của bạn)
    const char *backend_host = "127.0.0.1";
    int backend_port = 5500;

    printf("Starting reverse proxy...\n");
    printf("Frontend: http://127.0.0.1:%d/\n", listen_port);
    printf("Backend : http://%s:%d/\n", backend_host, backend_port);

    start_http_server(listen_port, backend_host, backend_port);
    return 0;
}
