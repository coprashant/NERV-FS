#ifndef NERVFS_SERVER_H
#define NERVFS_SERVER_H

/* shared constants used across the whole server */

#define NERVFS_DEFAULT_BACKLOG 16
#define NERVFS_BUFFER_SIZE 4096
#define NERVFS_STORAGE_ROOT "storage"
#define NERVFS_MAX_CLIENTS 1024
#define NERVFS_THREAD_POOL_SIZE 8

/* global running flag used for graceful shutdown, set up in phase 8 */
extern volatile int nervfs_running;

#endif