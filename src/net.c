#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "net.h"
#include "server.h"
#include "logging.h"

int net_create_listener(int port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("socket create failed");
        return -1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("setsockopt reuseaddr failed");
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("bind failed");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, NERVFS_DEFAULT_BACKLOG) < 0) {
        log_error("listen failed");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

void net_run_echo_loop(int listen_fd)
{
    log_info("phase 1 echo loop waiting for a client");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            log_error("accept failed");
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        char msg[128];
        snprintf(msg, sizeof(msg), "client connected from %s", ip_str);
        log_info(msg);

        char buffer[NERVFS_BUFFER_SIZE];
        ssize_t n;
        while ((n = read(client_fd, buffer, sizeof(buffer))) > 0) {
            ssize_t written = write(client_fd, buffer, (size_t)n);
            if (written < 0) {
                log_error("write to client failed");
                break;
            }
        }

        if (n < 0) {
            log_error("read from client failed");
        }

        close(client_fd);
        log_info("client disconnected");
    }
}
