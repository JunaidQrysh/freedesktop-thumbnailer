#include "progress.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static double elapsed_seconds(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) + (now.tv_nsec - start->tv_nsec) / 1e9;
}

void progress_init(progress_t *p, long total, int verbose) {
    atomic_init(&p->completed_ok, 0);
    atomic_init(&p->completed_skipped, 0);
    atomic_init(&p->completed_failed, 0);
    p->total = total;
    p->verbose = verbose;
    clock_gettime(CLOCK_MONOTONIC, &p->start_time);
    pthread_mutex_init(&p->print_lock, NULL);
}

void progress_redraw(progress_t *p) {
    long ok = atomic_load(&p->completed_ok);
    long skipped = atomic_load(&p->completed_skipped);
    long failed = atomic_load(&p->completed_failed);
    long done = ok + skipped + failed;
    double elapsed = elapsed_seconds(&p->start_time);
    double rate = elapsed > 0 ? done / elapsed : 0;

    pthread_mutex_lock(&p->print_lock);
    fprintf(stderr, "\r%ld/%ld done  (%ld ok, %ld skipped, %ld failed)  %.1f files/sec  %.1fs elapsed  ",
            done, p->total, ok, skipped, failed, rate, elapsed);
    fflush(stderr);
    pthread_mutex_unlock(&p->print_lock);
}

void progress_log(progress_t *p, const char *fmt, ...) {
    pthread_mutex_lock(&p->print_lock);
    /* clear the in-place progress line before printing a real message */
    fprintf(stderr, "\r\033[K");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    pthread_mutex_unlock(&p->print_lock);
}

void progress_finish_line(progress_t *p) {
    (void)p;
    fprintf(stderr, "\n");
}
