#ifndef NERVFS_THREADPOOL_H
#define NERVFS_THREADPOOL_H

#include "protocol.h"

/* a single unit of work, a client fd with its header already read */
/* the worker thread reads the payload itself once it dequeues the job */
typedef struct nervfs_job {
    int client_fd;
    nervfs_header_t header;
    struct nervfs_job *next;
} nervfs_job_t;

/* starts num_workers worker threads waiting on the job queue */
void threadpool_init(int num_workers);

/* queues a request whose header is already read, the pool takes ownership of client_fd */
void threadpool_submit(int client_fd, const nervfs_header_t *header);

/* signals all workers to stop and joins them */
void threadpool_shutdown(void);

#endif