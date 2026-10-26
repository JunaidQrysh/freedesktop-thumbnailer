#ifndef THUMBNAILER_H
#define THUMBNAILER_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ---- image_t / decoder registry ---------------------------------- */

typedef enum {
    PATH_DIRECT,       /* tier 1: stb_image direct decode */
    PATH_EMBEDDED,      /* tier 2: extracted embedded art/preview */
    PATH_FULL_DECODE     /* tier 3: full codec decode (libheif/libav) */
} decode_path_t;

typedef struct {
    uint8_t *pixels;   /* RGBA8, top-to-bottom, malloc'd, width*height*4 bytes */
    int width, height;  /* dimensions of `pixels` itself - what resize_to_fit operates on */
    int source_width, source_height; /* the REAL original media's dimensions, which for
                                        * embedded-preview formats (HEIC's EXIF thumbnail,
                                        * RAW previews) are NOT the same as width/height -
                                        * a 3000x4000 photo's embedded preview might only be
                                        * 256x192. Used for the "is this worth thumbnailing"
                                        * size check and for Thumb::Image::Width/Height, which
                                        * must describe the real original, not whichever small
                                        * buffer happened to be decoded. Every direct-decode
                                        * format just sets these equal to width/height. */
    decode_path_t path; /* which path was actually taken, for stats tagging */
} image_t;

/* Decoders return:
 *   1  -> success, *out filled in (out->pixels malloc'd, caller frees)
 *   0  -> no image available (expected "no art"/"no preview" outcome;
 *          audio/RAW formats only)
 *  -1  -> decode error (corrupt/unsupported file that should have had one)
 *
 * target_box is the requested thumbnail's bounding box (e.g. 128 for
 * "normal", 1024 for "xx-large"). Nearly every decoder ignores it - it
 * exists so a decoder with more than one possible source image (only
 * HEIC, currently: embedded EXIF thumbnail vs. full libheif decode) can
 * pick whichever is actually big enough for the requested size, instead
 * of committing to the cheap embedded thumbnail and then having the
 * engine discard the result as "too small" even though a properly-sized
 * thumbnail was available one tier up. */
typedef int (*decode_fn)(const char *path, image_t *out, int target_box);

typedef struct {
    const char *ext;   /* lowercase extension, no dot, e.g. "jpeg" */
    decode_fn fn;
} decoder_entry_t;

void image_free(image_t *img);

/* ---- key/value pairs for PNG tEXt chunks -------------------------- */

typedef struct {
    char key[64];
    char value[256];
} kv_pair_t;

/* ---- misc small helpers shared across modules ---------------------- */

const char *file_ext_lower(const char *path, char *buf, size_t buflen);

#endif
