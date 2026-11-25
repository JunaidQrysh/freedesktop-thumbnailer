#ifndef SWS_COLOR_H
#define SWS_COLOR_H

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

/* Tells swscale the real color range/matrix of the decoded frame.
 *
 * sws_getContext() picks full or limited range from the pixel format only
 * (yuvj* = full, everything else = limited). Hardware decoders output plain
 * NV12/P010 even for full-range content (phone HEIC photos are full-range
 * BT.601), so swscale treats it as limited range and stretches the contrast
 * by 255/219: crushed blacks, clipped highlights, oversaturated colors.
 * The frame (or the decoder, if the frame doesn't specify it) knows the
 * real values, so they are applied explicitly. */
static inline void sws_set_frame_colorspace(struct SwsContext* sws,
    const AVFrame* frame, const AVCodecContext* codec_ctx)
{
    if (!sws || !frame)
        return;

    enum AVColorRange range = frame->color_range;
    if (range == AVCOL_RANGE_UNSPECIFIED && codec_ctx)
        range = codec_ctx->color_range;
    enum AVColorSpace space = frame->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED && codec_ctx)
        space = codec_ctx->colorspace;

    int matrix = SWS_CS_ITU601;
    if (space == AVCOL_SPC_BT709)
        matrix = SWS_CS_ITU709;
    else if (space == AVCOL_SPC_BT2020_NCL || space == AVCOL_SPC_BT2020_CL)
        matrix = SWS_CS_BT2020;

    const int* coefs = sws_getCoefficients(matrix);
    const int src_full = (range == AVCOL_RANGE_JPEG);
    /* output is full-range RGB */
    sws_setColorspaceDetails(sws, coefs, src_full, coefs, 1, 0, 1 << 16, 1 << 16);
}

#endif /* SWS_COLOR_H */
