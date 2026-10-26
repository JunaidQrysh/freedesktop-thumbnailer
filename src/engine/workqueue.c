#include "workqueue.h"
#include <stdlib.h>
#include <string.h>

void wq_init(work_queue_t *q) {
    q->paths = NULL;
    q->total = 0;
    q->cap = 0;
    q->next = 0;
    pthread_mutex_init(&q->lock, NULL);
}

void wq_add(work_queue_t *q, const char *path) {
    /* single-threaded phase 1: growable array, geometric growth to avoid
     * O(n^2) reallocation on large trees. */
    if (q->total >= q->cap) {
        q->cap = q->cap == 0 ? 256 : q->cap * 2;
        q->paths = realloc(q->paths, q->cap * sizeof(char *));
    }
    q->paths[q->total] = strdup(path);
    q->total++;
}

void wq_finalize(work_queue_t *q) {
    (void)q; /* no-op: total is already fixed once phase 1 completes */
}

const char *wq_pop(work_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    const char *result = NULL;
    if (q->next < q->total) {
        result = q->paths[q->next];
        q->next++;
    }
    pthread_mutex_unlock(&q->lock);
    return result;
}

size_t wq_remaining(work_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    size_t r = (q->next < q->total) ? (q->total - q->next) : 0;
    pthread_mutex_unlock(&q->lock);
    return r;
}

void wq_destroy(work_queue_t *q) {
    for (size_t i = 0; i < q->total; i++) free(q->paths[i]);
    free(q->paths);
    pthread_mutex_destroy(&q->lock);
}
