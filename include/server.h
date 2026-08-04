#ifndef NERVFS_SERVER_H
#define NERVFS_SERVER_H

#include <signal.h>

/* shared constants used across the whole server */

#define NERVFS_DEFAULT_BACKLOG 16
#define NERVFS_BUFFER_SIZE 4096
#define NERVFS_STORAGE_ROOT "storage"
#define NERVFS_MAX_CLIENTS 1024
#define NERVFS_THREAD_POOL_SIZE 8

/* global running flag, set to zero by the SIGINT handler, checked by the select loop */
extern volatile sig_atomic_t nervfs_running;

#endif