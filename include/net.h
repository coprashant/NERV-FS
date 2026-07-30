#ifndef NERVFS_NET_H
#define NERVFS_NET_H

/* create listening socket bound to given port, returns fd or negative on error */
int net_create_listener(int port);

/* phase 1 blocking accept loop, accepts one client at a time and echoes input */
void net_run_echo_loop(int listen_fd);

/* phase 2 select based loop, watches the listener and all connected clients */
void net_run_select_loop(int listen_fd);

#endif