#ifndef STATS_H
#define STATS_H

/* Thread-safe accumulator: key -> {count, total_time, min, max} for timed
 * rows, key -> {count} for no-image/error rows. A single mutex guards the
 * (small, linearly-searched) table; contention is fine since this update
 * happens once per file, not per-pixel. */

void stats_init(void);
void stats_destroy(void);

/* Records a successful, timed decode for `key` (e.g. "jpeg",
 * "heic (embedded)"). seconds is wall-clock decode+resize+encode+write
 * time, not including queue wait time. */
void stats_record_timed(const char *key, double seconds);

/* Records a count-only outcome for `key` (e.g. "mp3 (no art)",
 * "mp3 (error)") - no timing associated. */
void stats_record_count(const char *key);

/* Prints the summary table (sorted by extension) to stdout, along with
 * walk/processing/total timing breakdown and the sum of per-file times.
 * If interrupted is non-zero, appends a note about the interrupted run
 * and how many files were left unprocessed. */
void stats_print_summary(double walk_time_s, double processing_time_s,
                          double total_time_s, int interrupted,
                          long files_remaining);

#endif
