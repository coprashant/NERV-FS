#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "server.h"
#include "net.h"
#include "logging.h"

volatile int nervfs_running = 1;

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage %s port\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port number\n");
        return 1;
    }

    printf("NERV-FS starting on port %d\n", port);

    int listen_fd = net_create_listener(port);
    if (listen_fd < 0) {
        fprintf(stderr, "failed to start listener\n");
        return 1;
    }

    log_info("listener ready");

    /* phase 1 blocking accept loop, will be replaced by select loop in phase 2 */
    net_run_echo_loop(listen_fd);

    close(listen_fd);
    return 0;
}
