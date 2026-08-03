#ifndef NERVFS_THREADPOOL_H
#define NERVFS_THREADPOOL_H

#include "protocol.h"

/* a single unit of work, a fully read client request waiting to be processed */
typedef struct nervfs_job {
    int client_fd;
    nervfs_header_t header;
    unsigned char *payload;
    struct nervfs_job *next;
} nervfs_job_t;

/* starts num_workers worker threads waiting on the job queue */
void threadpool_init(int num_workers);

/* queues a fully read request, the pool takes ownership of client_fd and payload */
void threadpool_submit(int client_fd, const nervfs_header_t *header, unsigned char *payload);

/* signals all workers to stop and joins them, used during phase 8 shutdown */
void threadpool_shutdown(void);

#endif