#include "exif_apply.h"
#include <stdlib.h>

static inline uint8_t *px_at(uint8_t *buf, int stride_w, int x, int y) {
    return buf + ((size_t)y * stride_w + x) * 4;
}

/* Each case below gives the destination (dx,dy) for source pixel (x,y) in
 * a w x h source image. Formulas verified by hand against small concrete
 * examples - see build notes. out_w/out_h are swapped from w/h for the
 * four 90-degree-involving cases (5,6,7,8). */
void exif_apply_orientation(image_t *img, int orientation) {
    if (orientation <= 1 || orientation > 8) return;

    int w = img->width, h = img->height;
    int out_w, out_h;
    switch (orientation) {
        case 5: case 6: case 7: case 8:
            out_w = h; out_h = w; break;
        default:
            out_w = w; out_h = h; break;
    }

    uint8_t *out = malloc((size_t)out_w * out_h * 4);
    if (!out) return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dx, dy;
            switch (orientation) {
                case 2: dx = w - 1 - x; dy = y;         break; /* flip horizontal */
                case 3: dx = w - 1 - x; dy = h - 1 - y; break; /* rotate 180 */
                case 4: dx = x;         dy = h - 1 - y; break; /* flip vertical */
                case 5: dx = y;         dy = x;         break; /* transpose */
                case 6: dx = h - 1 - y; dy = x;          break; /* rotate 90 CW */
                case 7: dx = h - 1 - y; dy = w - 1 - x;  break; /* transverse */
                case 8: dx = y;         dy = w - 1 - x;  break; /* rotate 90 CCW */
                default: dx = x; dy = y; break;
            }
            const uint8_t *src = px_at(img->pixels, w, x, y);
            uint8_t *dst = px_at(out, out_w, dx, dy);
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
    }

    free(img->pixels);
    img->pixels = out;
    img->width = out_w;
    img->height = out_h;
}
