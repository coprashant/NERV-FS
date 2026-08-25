#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "threadpool.h"
#include "protocol.h"
#include "server.h"
#include "logging.h"
#include "handlers.h"

static pthread_t workers[NERVFS_THREAD_POOL_SIZE];
static int active_workers = 0;

static nervfs_job_t *queue_head = NULL;
static nervfs_job_t *queue_tail = NULL;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static volatile int pool_running = 1;

/* runs the real file operation for a job */
static void process_job(nervfs_job_t *job)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "worker handling opcode %s for fd %d",
             opcode_name(job->header.opcode), job->client_fd);
    log_info(msg);

    // Record start time before dispatching the request
    struct timespec net_start;
    clock_gettime(CLOCK_MONOTONIC, &net_start);

    dispatch_request_timed(job->client_fd, &job->header, job->payload, net_start);

    close(job->client_fd);
}

static void *worker_main(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&queue_lock);

        while (queue_head == NULL && pool_running) {
            pthread_cond_wait(&queue_cond, &queue_lock);
        }

        if (queue_head == NULL && !pool_running) {
            pthread_mutex_unlock(&queue_lock);
            break;
        }

        nervfs_job_t *job = queue_head;
        queue_head = queue_head->next;
        if (queue_head == NULL) {
            queue_tail = NULL;
        }

        pthread_mutex_unlock(&queue_lock);

        process_job(job);
        free(job->payload);
        free(job);
    }

    return NULL;
}

void threadpool_init(int num_workers)
{
    if (num_workers > NERVFS_THREAD_POOL_SIZE) {
        num_workers = NERVFS_THREAD_POOL_SIZE;
    }

    for (int i = 0; i < num_workers; i++) {
        pthread_create(&workers[i], NULL, worker_main, NULL);
    }

    active_workers = num_workers;

    char msg[64];
    snprintf(msg, sizeof(msg), "thread pool started with %d workers", num_workers);
    log_info(msg);
}

void threadpool_submit(int client_fd, const nervfs_header_t *header, unsigned char *payload)
{
    nervfs_job_t *job = malloc(sizeof(nervfs_job_t));
    if (job == NULL) {
        log_error("job allocation failed");
        free(payload);
        close(client_fd);
        return;
    }

    job->client_fd = client_fd;
    job->header = *header;
    job->payload = payload;
    job->next = NULL;

    pthread_mutex_lock(&queue_lock);

    if (queue_tail == NULL) {
        queue_head = job;
        queue_tail = job;
    } else {
        queue_tail->next = job;
        queue_tail = job;
    }

    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_lock);
}

void threadpool_shutdown(void)
{
    pthread_mutex_lock(&queue_lock);
    pool_running = 0;
    pthread_cond_broadcast(&queue_cond);
    pthread_mutex_unlock(&queue_lock);

    for (int i = 0; i < active_workers; i++) {
        pthread_join(workers[i], NULL);
    }
}