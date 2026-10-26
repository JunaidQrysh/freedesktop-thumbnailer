#include "stats.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[40];
    long count;
    double total_time_s;
    double min_time_s;
    double max_time_s;
    int is_timed; /* 1 = timed row, 0 = count-only row */
} stat_entry_t;

#define MAX_ENTRIES 128
static stat_entry_t g_entries[MAX_ENTRIES];
static size_t g_n_entries = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void stats_init(void) {
    pthread_mutex_lock(&g_lock);
    g_n_entries = 0;
    pthread_mutex_unlock(&g_lock);
}

void stats_destroy(void) {
    /* nothing to free - static table */
}

static stat_entry_t *find_or_create(const char *key, int is_timed) {
    for (size_t i = 0; i < g_n_entries; i++) {
        if (strcmp(g_entries[i].key, key) == 0) return &g_entries[i];
    }
    if (g_n_entries >= MAX_ENTRIES) return NULL; /* shouldn't happen in practice */
    stat_entry_t *e = &g_entries[g_n_entries++];
    memset(e, 0, sizeof(*e));
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->is_timed = is_timed;
    e->min_time_s = -1;
    e->max_time_s = -1;
    return e;
}

void stats_record_timed(const char *key, double seconds) {
    pthread_mutex_lock(&g_lock);
    stat_entry_t *e = find_or_create(key, 1);
    if (e) {
        e->count++;
        e->total_time_s += seconds;
        if (e->min_time_s < 0 || seconds < e->min_time_s) e->min_time_s = seconds;
        if (e->max_time_s < 0 || seconds > e->max_time_s) e->max_time_s = seconds;
    }
    pthread_mutex_unlock(&g_lock);
}

void stats_record_count(const char *key) {
    pthread_mutex_lock(&g_lock);
    stat_entry_t *e = find_or_create(key, 0);
    if (e) e->count++;
    pthread_mutex_unlock(&g_lock);
}

static int cmp_key(const void *a, const void *b) {
    return strcmp(((const stat_entry_t *)a)->key, ((const stat_entry_t *)b)->key);
}

static void format_duration(double s, char *buf, size_t buflen) {
    if (s < 1.0) {
        snprintf(buf, buflen, "%.0fms", s * 1000.0);
    } else {
        snprintf(buf, buflen, "%.1fs", s);
    }
}

void stats_print_summary(double walk_time_s, double processing_time_s,
                          double total_time_s, int interrupted,
                          long files_remaining) {
    pthread_mutex_lock(&g_lock);

    stat_entry_t sorted[MAX_ENTRIES];
    memcpy(sorted, g_entries, sizeof(stat_entry_t) * g_n_entries);
    size_t n = g_n_entries;

    pthread_mutex_unlock(&g_lock);

    qsort(sorted, n, sizeof(stat_entry_t), cmp_key);

    long total_count = 0;
    double sum_per_file_time = 0;

    printf("\n%-18s %-8s %-8s %-8s %-8s %-8s\n", "ext", "count", "avg", "min", "max", "total");
    for (size_t i = 0; i < n; i++) {
        total_count += sorted[i].count;
        if (sorted[i].is_timed && sorted[i].count > 0) {
            sum_per_file_time += sorted[i].total_time_s;
            char avg_s[16], min_s[16], max_s[16], tot_s[16];
            format_duration(sorted[i].total_time_s / sorted[i].count, avg_s, sizeof(avg_s));
            format_duration(sorted[i].min_time_s, min_s, sizeof(min_s));
            format_duration(sorted[i].max_time_s, max_s, sizeof(max_s));
            format_duration(sorted[i].total_time_s, tot_s, sizeof(tot_s));
            printf("%-18s %-8ld %-8s %-8s %-8s %-8s\n",
                   sorted[i].key, sorted[i].count, avg_s, min_s, max_s, tot_s);
        } else {
            printf("%-18s %-8ld %-8s %-8s %-8s %-8s\n",
                   sorted[i].key, sorted[i].count, "-", "-", "-", "-");
        }
    }

    char total_dur[16];
    format_duration(sum_per_file_time, total_dur, sizeof(total_dur));
    printf("------------------------------------------------------------\n");
    printf("%-18s %-8ld %-8s %-8s %-8s %-8s\n", "total", total_count, "", "", "", total_dur);

    char walk_s[16], proc_s[16], tot_s[16], sum_s[16];
    format_duration(walk_time_s, walk_s, sizeof(walk_s));
    format_duration(processing_time_s, proc_s, sizeof(proc_s));
    format_duration(total_time_s, tot_s, sizeof(tot_s));
    format_duration(sum_per_file_time, sum_s, sizeof(sum_s));

    printf("\nwalk:        %s\n", walk_s);
    printf("processing:  %s   (wall clock, worker pool)\n", proc_s);
    printf("total:       %s\n", tot_s);
    printf("sum of per-file times: %s\n", sum_s);

    if (interrupted) {
        printf("\nRun interrupted (SIGTERM/SIGINT) - %ld file(s) left unprocessed in the queue.\n",
               files_remaining);
    }
}
