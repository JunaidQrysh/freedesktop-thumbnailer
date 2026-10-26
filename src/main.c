#include "engine/decoder_registry.h"
#include "engine/progress.h"
#include "engine/resize.h"
#include "engine/stats.h"
#include "engine/uri.h"
#include "engine/workqueue.h"
#include "engine/xdg.h"
#include "third_party/md5.h"
#include "third_party/png_writer.h"
#include "thumbnailer.h"

#include <dirent.h>
#include <errno.h>
#include <libavutil/log.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PATH_MAX_LOCAL 4096

/* ---- shutdown flag: signal-handler-safe, per spec's async-signal-safety
 * requirement (only touch a sig_atomic_t in the handler itself). ---- */
static volatile sig_atomic_t g_shutdown_requested = 0;

static void handle_signal(int sig)
{
    (void)sig;
    if (g_shutdown_requested) {
        _exit(1); /* second signal: force immediate exit, no graceful wait */
    }
    g_shutdown_requested = 1;
}

/* ---- size presets ---- */

typedef struct {
    const char* name;
    int box;
} size_preset_t;

static const size_preset_t SIZE_PRESETS[] = {
    { "normal", 128 },
    { "large", 256 },
    { "x-large", 512 },
    { "xx-large", 1024 },
};

/* ---- mimetype guesses, cheap to obtain from the extension we already
 * have on hand - not authoritative content sniffing, just good enough
 * for the "optional" Thumb::Mimetype key. ---- */

static const char* guess_mimetype(const char* ext)
{
    static const struct {
        const char* ext;
        const char* mime;
    } TABLE[] = {
        { "jpg", "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "png", "image/png" },
        { "bmp", "image/bmp" },
        { "gif", "image/gif" },
        { "tga", "image/x-tga" },
        { "psd", "image/vnd.adobe.photoshop" },
        { "pnm", "image/x-portable-anymap" },
        { "pgm", "image/x-portable-graymap" },
        { "ppm", "image/x-portable-pixmap" },
        { "hdr", "image/vnd.radiance" },
        { "heic", "image/heic" },
        { "heif", "image/heif" },
        { "mp3", "audio/mpeg" },
        { "flac", "audio/flac" },
        { "ogg", "audio/ogg" },
        { "opus", "audio/ogg" },
        { "m4a", "audio/mp4" },
        { "cr2", "image/x-canon-cr2" },
        { "nef", "image/x-nikon-nef" },
        { "arw", "image/x-sony-arw" },
        { "dng", "image/x-adobe-dng" },
        { "mp4", "video/mp4" },
        { "mkv", "video/x-matroska" },
        { "webm", "video/webm" },
        { "mov", "video/quicktime" },
        { "avi", "video/x-msvideo" },
        { "3gp", "video/3gpp" },
    };
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(TABLE[i].ext, ext) == 0)
            return TABLE[i].mime;
    }
    return NULL;
}

/* Determines if a format uses hardware-accelerated video decoding.
 * These formats are routed to the smaller GPU thread pool to prevent
 * overwhelming the hardware decode silicon. */
static int is_gpu_bound(const char* ext)
{
    if (strcmp(ext, "heic") == 0 || strcmp(ext, "heif") == 0)
        return 1;
    if (strcmp(ext, "mp4") == 0 || strcmp(ext, "mkv") == 0)
        return 1;
    if (strcmp(ext, "webm") == 0 || strcmp(ext, "mov") == 0)
        return 1;
    if (strcmp(ext, "avi") == 0 || strcmp(ext, "3gp") == 0)
        return 1;
    return 0;
}

/* Determines if a file manager requires a generated PNG thumbnail
 * regardless of the original file's resolution. */
static int requires_thumbnail_always(const char* ext)
{
    /* Videos */
    if (strcmp(ext, "mp4") == 0 || strcmp(ext, "mkv") == 0 || strcmp(ext, "webm") == 0 || strcmp(ext, "mov") == 0 || strcmp(ext, "avi") == 0 || strcmp(ext, "3gp") == 0)
        return 1;
    /* Audio (Cover Art) */
    if (strcmp(ext, "mp3") == 0 || strcmp(ext, "flac") == 0 || strcmp(ext, "m4a") == 0 || strcmp(ext, "ogg") == 0)
        return 1;
    /* RAW Photos */
    if (strcmp(ext, "cr2") == 0 || strcmp(ext, "nef") == 0 || strcmp(ext, "arw") == 0 || strcmp(ext, "dng") == 0)
        return 1;

    return 0;
}

/* ---- directory setup with strict permission enforcement ---- */

/* Creates intermediate path components ABOVE the final one (like a plain
 * "mkdir -p" on the dirname only) using a permissive default mode - used
 * only for parent directories ABOVE the thumbnails tree itself (e.g.
 * ~/.cache), which the spec doesn't impose a permission requirement on.
 * Deliberately does NOT create the final path component itself - that's
 * ensure_thumb_dir_secure's job, at the stricter 700 mode. */
static int mkdir_p_lenient_parent_of(const char* path)
{
    char tmp[PATH_MAX_LOCAL];
    size_t len = strlen(path);
    if (len >= sizeof(tmp))
        return -1;
    strcpy(tmp, path);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    return 0; /* stop before creating `path` itself */
}

/* Enforces the spec's strict rule for the thumbnails dir and its defined
 * subdirectories: if it exists, its mode must be exactly 700, else this
 * is a fatal error (untrusted directory - refuse rather than guess). If
 * it doesn't exist, create it at 700. */
static int ensure_thumb_dir_secure(const char* path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "error: %s exists and is not a directory\n", path);
            return -1;
        }
        mode_t mode = st.st_mode & 0777;
        if (mode != 0700) {
            fprintf(stderr,
                "error: %s has permissions %o, expected 700 - refusing to proceed "
                "(this could mean another process created it; fix permissions manually)\n",
                path, mode);
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        fprintf(stderr, "error: cannot stat %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (mkdir(path, 0700) != 0) {
        fprintf(stderr, "error: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (chmod(path, 0700) != 0) { /* defeat umask */
        fprintf(stderr, "error: cannot chmod %s to 700: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- walk phase ---- */

static int is_inside_thumbnails_dir(const char* canonical_uri, const char* thumbs_base_uri)
{
    size_t blen = strlen(thumbs_base_uri);
    return strncmp(canonical_uri, thumbs_base_uri, blen) == 0 && (canonical_uri[blen] == '/' || canonical_uri[blen] == '\0');
}

static void maybe_enqueue(const char* path, work_queue_t* cpu_q, work_queue_t* gpu_q, const char* thumbs_base_uri)
{
    char extbuf[16];
    const char* ext = file_ext_lower(path, extbuf, sizeof(extbuf));
    if (!ext || !registry_has(ext))
        return;

    char* uri = canonical_file_uri(path);
    if (!uri)
        return;
    if (is_inside_thumbnails_dir(uri, thumbs_base_uri)) {
        free(uri);
        return;
    }
    free(uri);

    /* Route the file to the correct thread pool */
    if (is_gpu_bound(ext)) {
        wq_add(gpu_q, path);
    } else {
        wq_add(cpu_q, path);
    }
}

static void walk_dir(const char* dirpath, int recurse, work_queue_t* cpu_q, work_queue_t* gpu_q, const char* thumbs_base_uri)
{
    DIR* d = opendir(dirpath);
    if (!d)
        return;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child[PATH_MAX_LOCAL];
        snprintf(child, sizeof(child), "%s/%s", dirpath, entry->d_name);

        struct stat st;
        if (lstat(child, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (recurse)
                walk_dir(child, recurse, cpu_q, gpu_q, thumbs_base_uri);
        } else if (S_ISREG(st.st_mode)) {
            maybe_enqueue(child, cpu_q, gpu_q, thumbs_base_uri);
        }
    }
    closedir(d);
}

static void walk_input(const char* input, int recurse, work_queue_t* cpu_q, work_queue_t* gpu_q, const char* thumbs_base_uri)
{
    struct stat st;
    if (stat(input, &st) != 0) {
        fprintf(stderr, "warning: cannot stat %s: %s\n", input, strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        walk_dir(input, recurse, cpu_q, gpu_q, thumbs_base_uri);
    } else if (S_ISREG(st.st_mode)) {
        maybe_enqueue(input, cpu_q, gpu_q, thumbs_base_uri);
    }
}

/* ---- per-file pipeline ---- */

typedef struct {
    work_queue_t* queue;
    const char* thumbs_base; /* filesystem path, e.g. /home/x/.cache/thumbnails */
    const char* size_dir_name; /* "normal" / "large" / "x-large" / "xx-large" */
    int target_box;
    int verbose;
    progress_t* progress;
} worker_ctx_t;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Checks whether a valid (URI+MTime matching) thumbnail/marker already
 * exists at `png_path`. Returns 1 if valid (skip regeneration), 0
 * otherwise. */
static int existing_thumbnail_is_valid(const char* png_path, const char* uri, const char* mtime_str)
{
    kv_pair_t kv[8];
    int n = png_read_text_chunks(png_path, kv, 8);
    if (n <= 0)
        return 0;

    const char *found_uri = NULL, *found_mtime = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(kv[i].key, "Thumb::URI") == 0)
            found_uri = kv[i].value;
        if (strcmp(kv[i].key, "Thumb::MTime") == 0)
            found_mtime = kv[i].value;
    }
    return found_uri && found_mtime && strcmp(found_uri, uri) == 0 && strcmp(found_mtime, mtime_str) == 0;
}

/* Writes `pixels`/dims (or a 1x1 transparent pixel if pixels is NULL, for
 * fail markers) atomically into `dest_dir/hash.png` via a same-directory
 * temp file + rename, with 0600 permissions. */
static int atomic_write_png(const char* dest_dir, const char* hash,
    const uint8_t* pixels, int width, int height,
    const kv_pair_t* kv, int n_kv)
{
    uint8_t fallback_px[4] = { 0, 0, 0, 0 };
    if (!pixels) {
        pixels = fallback_px;
        width = 1;
        height = 1;
    }

    char tmp_path[PATH_MAX_LOCAL];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.thumbnailer-%d-%.8s.tmp",
        dest_dir, (int)getpid(), hash);

    if (png_write_rgba(tmp_path, pixels, width, height, kv, n_kv) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (chmod(tmp_path, 0600) != 0) {
        unlink(tmp_path);
        return -1;
    }

    char final_path[PATH_MAX_LOCAL];
    snprintf(final_path, sizeof(final_path), "%s/%s.png", dest_dir, hash);
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static void process_one_file(worker_ctx_t* ctx, const char* path)
{
    char extbuf[16];
    const char* ext = file_ext_lower(path, extbuf, sizeof(extbuf));
    if (!ext)
        return; /* shouldn't happen - walk already filtered */

    /* Permissions gate: don't touch the cache at all if unreadable. */
    if (access(path, R_OK) != 0) {
        char key[40];
        snprintf(key, sizeof(key), "%s (unreadable)", ext);
        stats_record_count(key);
        atomic_fetch_add(&ctx->progress->completed_skipped, 1);
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        char key[40];
        snprintf(key, sizeof(key), "%s (unreadable)", ext);
        stats_record_count(key);
        atomic_fetch_add(&ctx->progress->completed_skipped, 1);
        return;
    }

    char* uri = canonical_file_uri(path);
    if (!uri) {
        char key[40];
        snprintf(key, sizeof(key), "%s (error)", ext);
        stats_record_count(key);
        atomic_fetch_add(&ctx->progress->completed_failed, 1);
        progress_log(ctx->progress, "FAIL  %s (uri error)", path);
        return;
    }

    char mtime_str[32];
    snprintf(mtime_str, sizeof(mtime_str), "%ld", (long)st.st_mtime);

    char hash[33];
    md5_hex_string(uri, hash);

    char size_dir[PATH_MAX_LOCAL];
    snprintf(size_dir, sizeof(size_dir), "%s/%s", ctx->thumbs_base, ctx->size_dir_name);
    char fail_dir[PATH_MAX_LOCAL];
    snprintf(fail_dir, sizeof(fail_dir), "%s/fail/thumbnailer-v1", ctx->thumbs_base);

    char success_path[PATH_MAX_LOCAL], fail_path[PATH_MAX_LOCAL];
    snprintf(success_path, sizeof(success_path), "%s/%s.png", size_dir, hash);
    snprintf(fail_path, sizeof(fail_path), "%s/%s.png", fail_dir, hash);

    if (existing_thumbnail_is_valid(success_path, uri, mtime_str) || existing_thumbnail_is_valid(fail_path, uri, mtime_str)) {
        char key[40];
        snprintf(key, sizeof(key), "%s (cached)", ext);
        stats_record_count(key);
        atomic_fetch_add(&ctx->progress->completed_skipped, 1);
        free(uri);
        return;
    }

    kv_pair_t kv[8];
    int n_kv = 0;
    strcpy(kv[n_kv].key, "Thumb::URI");
    strncpy(kv[n_kv].value, uri, sizeof(kv[n_kv].value) - 1);
    kv[n_kv].value[sizeof(kv[n_kv].value) - 1] = 0;
    n_kv++;
    strcpy(kv[n_kv].key, "Thumb::MTime");
    strcpy(kv[n_kv].value, mtime_str);
    n_kv++;

    double t0 = now_seconds();
    decode_fn fn = registry_find(ext);
    image_t img = { 0 };
    int rc = fn ? fn(path, &img, ctx->target_box) : -1;

    if (rc <= 0) {
        char key[40];
        snprintf(key, sizeof(key), "%s (error)", ext);
        stats_record_count(key);

        if (rc == 0) {
            atomic_fetch_add(&ctx->progress->completed_skipped, 1);
        } else {
            atomic_write_png(fail_dir, hash, NULL, 0, 0, kv, n_kv);
            atomic_fetch_add(&ctx->progress->completed_failed, 1);
        }
        free(uri);
        return;
    }

    int source_w = img.source_width, source_h = img.source_height;
    decode_path_t which_path = img.path;

    if (!requires_thumbnail_always(ext) && source_w <= ctx->target_box && source_h <= ctx->target_box) {
        char key[40];
        snprintf(key, sizeof(key), "%s (too small)", ext);
        stats_record_count(key);
        atomic_fetch_add(&ctx->progress->completed_skipped, 1);
        image_free(&img);
        free(uri);
        return;
    }

    image_t resized;
    resize_to_fit(&img, ctx->target_box, ctx->target_box, &resized);
    image_free(&img);

    char widthbuf[16], heightbuf[16], sizebuf[24];
    snprintf(widthbuf, sizeof(widthbuf), "%d", source_w);
    snprintf(heightbuf, sizeof(heightbuf), "%d", source_h);
    snprintf(sizebuf, sizeof(sizebuf), "%lld", (long long)st.st_size);

    strcpy(kv[n_kv].key, "Thumb::Size");
    strcpy(kv[n_kv].value, sizebuf);
    n_kv++;
    strcpy(kv[n_kv].key, "Thumb::Image::Width");
    strcpy(kv[n_kv].value, widthbuf);
    n_kv++;
    strcpy(kv[n_kv].key, "Thumb::Image::Height");
    strcpy(kv[n_kv].value, heightbuf);
    n_kv++;
    const char* mime = guess_mimetype(ext);
    if (mime) {
        strcpy(kv[n_kv].key, "Thumb::Mimetype");
        strcpy(kv[n_kv].value, mime);
        n_kv++;
    }
    strcpy(kv[n_kv].key, "Software");
    strcpy(kv[n_kv].value, "thumbnailer v1");
    n_kv++;

    int write_rc = atomic_write_png(size_dir, hash, resized.pixels, resized.width, resized.height, kv, n_kv);

    image_free(&resized);
    free(uri);

    double elapsed = now_seconds() - t0;

    if (write_rc != 0) {
        char key[40];
        snprintf(key, sizeof(key), "%s (error)", ext);
        stats_record_count(key);
        atomic_fetch_add(&ctx->progress->completed_failed, 1);
        progress_log(ctx->progress, "FAIL  %s (write error)", path);
        return;
    }

    char key[40];
    if (strcmp(ext, "heic") == 0 || strcmp(ext, "heif") == 0) {
        snprintf(key, sizeof(key), "%s (%s)", ext,
            which_path == PATH_EMBEDDED ? "embedded" : "full-decode");
    } else {
        snprintf(key, sizeof(key), "%s", ext);
    }
    stats_record_timed(key, elapsed);
    atomic_fetch_add(&ctx->progress->completed_ok, 1);
    if (ctx->verbose)
        progress_log(ctx->progress, "OK    %s (%.0fms)", path, elapsed * 1000.0);
}

static void* worker_thread(void* arg)
{
    worker_ctx_t* ctx = (worker_ctx_t*)arg;
    for (;;) {
        if (g_shutdown_requested)
            break;
        const char* path = wq_pop(ctx->queue);
        if (!path)
            break;
        process_one_file(ctx, path);
    }
    return NULL;
}

typedef struct {
    progress_t* progress;
    volatile int* done;
} reporter_ctx_t;

static void* reporter_thread(void* arg)
{
    reporter_ctx_t* ctx = (reporter_ctx_t*)arg;
    while (!*ctx->done) {
        progress_redraw(ctx->progress);
        usleep(150000);
    }
    progress_redraw(ctx->progress);
    return NULL;
}

/* ---- main ---- */

static void usage(const char* prog)
{
    fprintf(stderr, "usage: %s [-r] [-j N] [-s normal|large|x-large|xx-large] [-v] <file-or-directory> [...]\n", prog);
}

int main(int argc, char** argv)
{
    int recurse = 0;
    int verbose = 0;
    const char* size_name = "normal";

    /* Default pool sizes */
    int cpu_workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_workers < 1)
        cpu_workers = 1;
    int gpu_workers = 4; /* Safe clamp for hardware video decoders */

    int opt;
    while ((opt = getopt(argc, argv, "rj:s:v")) != -1) {
        switch (opt) {
        case 'r':
            recurse = 1;
            break;
        case 'j':
            cpu_workers = atoi(optarg);
            if (cpu_workers < 1)
                cpu_workers = 1;
            /* If the user explicitly asks for fewer threads overall, don't let
             * the GPU queue spawn more threads than the CPU queue. */
            if (gpu_workers > cpu_workers)
                gpu_workers = cpu_workers;
            break;
        case 's':
            size_name = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    int target_box = -1;
    for (size_t i = 0; i < sizeof(SIZE_PRESETS) / sizeof(SIZE_PRESETS[0]); i++) {
        if (strcmp(SIZE_PRESETS[i].name, size_name) == 0) {
            target_box = SIZE_PRESETS[i].box;
            break;
        }
    }
    if (target_box < 0) {
        fprintf(stderr, "error: invalid size '%s' (expected normal|large|x-large|xx-large)\n", size_name);
        return 1;
    }

    char* thumbs_base = thumbnails_base_dir();
    if (!thumbs_base) {
        fprintf(stderr, "error: could not determine thumbnails directory (HOME unset?)\n");
        return 1;
    }

    mkdir_p_lenient_parent_of(thumbs_base);
    if (ensure_thumb_dir_secure(thumbs_base) != 0) {
        free(thumbs_base);
        return 1;
    }

    const char* subdirs[] = { "normal", "large", "x-large", "xx-large", "fail" };
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        char p[PATH_MAX_LOCAL];
        snprintf(p, sizeof(p), "%s/%s", thumbs_base, subdirs[i]);
        if (ensure_thumb_dir_secure(p) != 0) {
            free(thumbs_base);
            return 1;
        }
    }
    char fail_v1[PATH_MAX_LOCAL];
    snprintf(fail_v1, sizeof(fail_v1), "%s/fail/thumbnailer-v1", thumbs_base);
    if (ensure_thumb_dir_secure(fail_v1) != 0) {
        free(thumbs_base);
        return 1;
    }

    char* thumbs_base_uri = canonical_file_uri(thumbs_base);
    if (!thumbs_base_uri) {
        fprintf(stderr, "error: could not canonicalize thumbnails dir\n");
        free(thumbs_base);
        return 1;
    }

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    stats_init();
    av_log_set_level(AV_LOG_FATAL);

    /* ---- Phase 1: walk ---- */
    double walk_start = now_seconds();
    work_queue_t cpu_queue, gpu_queue;
    wq_init(&cpu_queue);
    wq_init(&gpu_queue);

    for (int i = optind; i < argc; i++) {
        walk_input(argv[i], recurse, &cpu_queue, &gpu_queue, thumbs_base_uri);
    }
    wq_finalize(&cpu_queue);
    wq_finalize(&gpu_queue);
    double walk_time = now_seconds() - walk_start;

    size_t total_files = cpu_queue.total + gpu_queue.total;
    fprintf(stderr, "found %zu file(s) to process (%zu CPU-bound, %zu GPU-bound)\n",
        total_files, cpu_queue.total, gpu_queue.total);

    if (total_files == 0) {
        wq_destroy(&cpu_queue);
        wq_destroy(&gpu_queue);
        stats_destroy();
        free(thumbs_base);
        free(thumbs_base_uri);
        return 0;
    }

    /* ---- Phase 2: process ---- */
    progress_t progress;
    progress_init(&progress, (long)total_files, verbose);

    worker_ctx_t cpu_ctx = {
        .queue = &cpu_queue,
        .thumbs_base = thumbs_base,
        .size_dir_name = size_name,
        .target_box = target_box,
        .verbose = verbose,
        .progress = &progress,
    };

    worker_ctx_t gpu_ctx = {
        .queue = &gpu_queue,
        .thumbs_base = thumbs_base,
        .size_dir_name = size_name,
        .target_box = target_box,
        .verbose = verbose,
        .progress = &progress,
    };

    double proc_start = now_seconds();

    /* Spawn CPU pool */
    pthread_t* cpu_threads = malloc(sizeof(pthread_t) * cpu_workers);
    for (int i = 0; i < cpu_workers; i++) {
        pthread_create(&cpu_threads[i], NULL, worker_thread, &cpu_ctx);
    }

    /* Spawn GPU pool */
    pthread_t* gpu_threads = malloc(sizeof(pthread_t) * gpu_workers);
    for (int i = 0; i < gpu_workers; i++) {
        pthread_create(&gpu_threads[i], NULL, worker_thread, &gpu_ctx);
    }

    /* Spawn Reporter */
    volatile int reporter_done = 0;
    reporter_ctx_t rctx = { &progress, &reporter_done };
    pthread_t reporter;
    pthread_create(&reporter, NULL, reporter_thread, &rctx);

    /* Wait for all workers to finish */
    for (int i = 0; i < cpu_workers; i++) {
        pthread_join(cpu_threads[i], NULL);
    }
    for (int i = 0; i < gpu_workers; i++) {
        pthread_join(gpu_threads[i], NULL);
    }

    reporter_done = 1;
    pthread_join(reporter, NULL);
    progress_finish_line(&progress);

    double proc_time = now_seconds() - proc_start;
    double total_time = walk_time + proc_time;

    long remaining = (long)(wq_remaining(&cpu_queue) + wq_remaining(&gpu_queue));
    stats_print_summary(walk_time, proc_time, total_time, g_shutdown_requested, remaining);

    free(cpu_threads);
    free(gpu_threads);
    wq_destroy(&cpu_queue);
    wq_destroy(&gpu_queue);
    stats_destroy();
    free(thumbs_base);
    free(thumbs_base_uri);

    return 0;
}
