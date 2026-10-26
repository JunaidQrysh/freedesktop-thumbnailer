#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdatomic.h>
#include <pthread.h>

typedef struct {
    atomic_long completed_ok;
    atomic_long completed_skipped;
    atomic_long completed_failed;
    long total;
    struct timespec start_time;
    pthread_mutex_t print_lock; /* serializes ALL stdout/stderr writes:
                                    progress redraws, verbose lines, errors */
    int verbose;
} progress_t;

void progress_init(progress_t *p, long total, int verbose);

/* Prints a single in-place (\r) progress line: done/total, rate, elapsed.
 * Safe to call from a timer thread; takes the print lock. */
void progress_redraw(progress_t *p);

/* Serialized print helpers for worker threads - clears the current
 * progress line, prints the message, and leaves the terminal ready for
 * the next redraw. */
void progress_log(progress_t *p, const char *fmt, ...);

void progress_finish_line(progress_t *p); /* newline after final redraw */

#endif
