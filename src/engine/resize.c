#include "resize.h"
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

static void box_downscale(const uint8_t* src, int sw, int sh, int dw, int dh, uint8_t* dst)
{
    /* FFmpeg's assembly-optimized fast bilinear scaler */
    struct SwsContext* sws = sws_getContext(
        sw, sh, AV_PIX_FMT_RGBA,
        dw, dh, AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);

    if (sws) {
        const uint8_t* src_data[4] = { src, NULL, NULL, NULL };
        int src_linesize[4] = { sw * 4, 0, 0, 0 };

        uint8_t* dst_data[4] = { dst, NULL, NULL, NULL };
        int dst_linesize[4] = { dw * 4, 0, 0, 0 };

        sws_scale(sws, src_data, src_linesize, 0, sh, dst_data, dst_linesize);
        sws_freeContext(sws);
    }
}

void resize_to_fit(const image_t* src, int max_w, int max_h, image_t* out)
{
    double scale = 1.0;
    if (src->width > max_w || src->height > max_h) {
        double sx = (double)max_w / src->width;
        double sy = (double)max_h / src->height;
        scale = sx < sy ? sx : sy;
    }
    /* never upscale */
    if (scale > 1.0)
        scale = 1.0;

    int dw = (int)(src->width * scale + 0.5);
    int dh = (int)(src->height * scale + 0.5);
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;

    out->width = dw;
    out->height = dh;
    out->path = src->path;
    out->pixels = malloc((size_t)dw * dh * 4);

    if (dw == src->width && dh == src->height) {
        memcpy(out->pixels, src->pixels, (size_t)dw * dh * 4);
    } else {
        box_downscale(src->pixels, src->width, src->height, dw, dh, out->pixels);
    }
}
