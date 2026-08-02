#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "net.h"
#include "server.h"
#include "logging.h"
#include "protocol.h"

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

/* handles one readable client, returns 0 if still connected 1 if it should be closed */
static int handle_client_readable(int client_fd)
{
    unsigned char header_buf[NERVFS_HEADER_SIZE];
    ssize_t got = recv_full(client_fd, header_buf, NERVFS_HEADER_SIZE);

    if (got == 0) {
        log_info("client disconnected");
        return 1;
    }

    if (got < 0 || (size_t)got < NERVFS_HEADER_SIZE) {
        log_error("failed to read full header");
        return 1;
    }

    nervfs_header_t header;
    if (parse_header(header_buf, &header) < 0) {
        log_info("bad magic or version rejecting client");
        return 1;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "received opcode %s payload len %u",
             opcode_name(header.opcode), header.payload_len);
    log_info(msg);

    if (header.payload_len > 0) {
        unsigned char *payload = malloc(header.payload_len);
        if (payload == NULL) {
            log_error("payload allocation failed");
            return 1;
        }

        ssize_t pgot = recv_full(client_fd, payload, header.payload_len);
        if (pgot < 0 || (uint32_t)pgot < header.payload_len) {
            log_error("failed to read full payload");
            free(payload);
            return 1;
        }

        free(payload);
        /* opcode dispatch to real handlers gets added in phase 5 */
    }

    return 0;
}

void net_run_select_loop(int listen_fd)
{
    int client_fds[NERVFS_MAX_CLIENTS];
    for (int i = 0; i < NERVFS_MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }

    log_info("phase 2 select loop watching listener and clients");

    while (1) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listen_fd, &read_set);
        int max_fd = listen_fd;

        for (int i = 0; i < NERVFS_MAX_CLIENTS; i++) {
            if (client_fds[i] != -1) {
                FD_SET(client_fds[i], &read_set);
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }

        int ready = select(max_fd + 1, &read_set, NULL, NULL, NULL);
        if (ready < 0) {
            log_error("select failed");
            continue;
        }

        if (FD_ISSET(listen_fd, &read_set)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

            if (new_fd < 0) {
                log_error("accept failed");
            } else {
                int slot = -1;
                for (int i = 0; i < NERVFS_MAX_CLIENTS; i++) {
                    if (client_fds[i] == -1) {
                        slot = i;
                        break;
                    }
                }

                if (slot == -1) {
                    log_info("client rejected, max clients reached");
                    close(new_fd);
                } else {
                    client_fds[slot] = new_fd;
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
                    char msg[128];
                    snprintf(msg, sizeof(msg), "client connected from %s", ip_str);
                    log_info(msg);
                }
            }
        }

        for (int i = 0; i < NERVFS_MAX_CLIENTS; i++) {
            int fd = client_fds[i];
            if (fd != -1 && FD_ISSET(fd, &read_set)) {
                if (handle_client_readable(fd)) {
                    close(fd);
                    client_fds[i] = -1;
                }
            }
        }
    }
}