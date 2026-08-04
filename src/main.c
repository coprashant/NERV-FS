#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "server.h"
#include "net.h"
#include "logging.h"
#include "threadpool.h"
#include "fileops.h"

volatile sig_atomic_t nervfs_running = 1;

/* only sets a flag here, printf and friends are not safe to call from a signal handler */
static void handle_sigint(int sig)
{
    (void)sig;
    nervfs_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = handle_sigint;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, NULL);
}

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

    install_signal_handlers();

    if (fileops_init(NERVFS_STORAGE_ROOT) < 0) {
        fprintf(stderr, "failed to set up storage root\n");
        return 1;
    }

    int listen_fd = net_create_listener(port);
    if (listen_fd < 0) {
        fprintf(stderr, "failed to start listener\n");
        return 1;
    }

    log_info("listener ready");

    threadpool_init(NERVFS_THREAD_POOL_SIZE);

    /* phase 2 select loop, returns once nervfs_running is cleared by the SIGINT handler */
    net_run_select_loop(listen_fd);

    log_info("select loop stopped, shutting down thread pool");
    threadpool_shutdown();

    close(listen_fd);
    log_info("shutdown complete");
    return 0;
}