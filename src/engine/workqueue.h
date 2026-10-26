#ifndef WORKQUEUE_H
#define WORKQUEUE_H

#include <stddef.h>
#include <pthread.h>

typedef struct {
    char **paths;      /* malloc'd array of malloc'd strings */
    size_t total;
    size_t cap;         /* allocated capacity of paths[], phase-1 only */
    size_t next;        /* index of next unclaimed item */
    pthread_mutex_t lock;
} work_queue_t;

void wq_init(work_queue_t *q);
void wq_add(work_queue_t *q, const char *path); /* phase 1 only, single-threaded */
void wq_finalize(work_queue_t *q);               /* no more adds after this */

/* Returns a path (caller does NOT own/free it - queue owns it) or NULL if
 * exhausted. Thread-safe. */
const char *wq_pop(work_queue_t *q);

size_t wq_remaining(work_queue_t *q); /* approximate, for interrupted-run reporting */

void wq_destroy(work_queue_t *q);

#endif
