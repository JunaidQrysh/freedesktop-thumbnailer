#ifndef EXIF_APPLY_H
#define EXIF_APPLY_H

#include "../thumbnailer.h"

/* Applies standard EXIF orientation values 1-8 to img in place (may
 * reallocate img->pixels and swap width/height for the 90-degree cases).
 * orientation == 1 is a no-op. */
void exif_apply_orientation(image_t *img, int orientation);

#endif
