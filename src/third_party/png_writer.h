#ifndef PNG_WRITER_H
#define PNG_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include "../thumbnailer.h"

/* Writes an 8-bit, non-interlaced RGBA PNG with the given tEXt chunks to
 * `path`. pixels must be width*height*4 bytes, top-to-bottom, RGBA8.
 * Returns 0 on success, -1 on failure (errno set by the failing syscall
 * where applicable). Uses zlib for DEFLATE compression and CRC32 (chunk
 * framing itself is hand-rolled, not libpng). */
int png_write_rgba(const char *path, const uint8_t *pixels, int width,
                    int height, const kv_pair_t *kv, int n_kv);

/* Reads just the tEXt chunks out of an existing PNG at `path` (does not
 * decompress IDAT - only chunk headers are walked, IDAT/other chunk data
 * is skipped over with fseek). Writes up to max_kv entries into kv,
 * returns the number found, or -1 if the file doesn't exist / isn't a
 * PNG / can't be read. Used only for the existing-thumbnail validity
 * check (comparing Thumb::URI / Thumb::MTime), not general PNG reading. */
int png_read_text_chunks(const char *path, kv_pair_t *kv, int max_kv);

#endif
