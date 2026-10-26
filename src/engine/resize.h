#ifndef RESIZE_H
#define RESIZE_H

#include "../thumbnailer.h"

/* Resizes src to fit inside max_w x max_h, preserving aspect ratio, never
 * upscaling beyond the source's own dimensions. Fills *out with a newly
 * malloc'd RGBA8 buffer (caller frees via image_free). If the source
 * already fits within max_w x max_h, out is a plain copy of src at its
 * original size (no upscale). Uses a box filter when shrinking (area
 * averaging), which is a reasonable general-purpose downscale filter for
 * arbitrary shrink ratios. */
void resize_to_fit(const image_t *src, int max_w, int max_h, image_t *out);

#endif
