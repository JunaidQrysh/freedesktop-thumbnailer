/* Video thumbnail: open with libavformat, find the best video stream,
 * seek to ~10% into the duration (a frame at time 0 is very often a
 * black/blank slate for real-world video, so a small offset in tends to
 * give a much more representative thumbnail), decode forward to the
 * next available frame, and convert it to RGBA via libswscale. Single
 * decode path always - no embedded-art pre-check tier for video. */
#include "formats.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

int decode_video(const char* path, image_t* out, int target_box)
{
    (void)target_box;
    AVFormatContext* fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) != 0)
        return -1;

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream_idx < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    AVStream* stream = fmt_ctx->streams[stream_idx];

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }
    if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0 || avcodec_open2(codec_ctx, codec, NULL) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    /* seek ~10% into the stream, best-effort - if duration is unknown or
     * the seek fails, just proceed reading from wherever we are (start) */
    if (fmt_ctx->duration > 0) {
        int64_t seek_target = fmt_ctx->duration / 10;
        av_seek_frame(fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int result = -1;

    while (result != 1 && av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                if (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    int w = frame->width, h = frame->height;
                    struct SwsContext* sws = sws_getContext(
                        w, h, (enum AVPixelFormat)frame->format,
                        w, h, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, NULL, NULL, NULL);
                    if (sws) {
                        /* sws_scale's SIMD (SSE/AVX) write paths can write
                         * a few bytes past the end of each row for certain
                         * widths - FFmpeg's documented requirement is that
                         * sws_scale output buffers be allocated with
                         * av_image_alloc (which pads/aligns each linesize
                         * appropriately), never a bare malloc(w*h*bpp).
                         * Skipping this causes intermittent heap
                         * corruption (glibc "free(): invalid next size")
                         * on some later, unrelated free() call - the
                         * corruption doesn't surface at the point of the
                         * overrun itself. */
                        uint8_t* padded_data[4] = { NULL, NULL, NULL, NULL };
                        int padded_linesize[4] = { 0, 0, 0, 0 };
                        int alloc_rc = av_image_alloc(padded_data, padded_linesize,
                            w, h, AV_PIX_FMT_RGBA, 32);
                        if (alloc_rc >= 0) {
                            sws_scale(sws, (const uint8_t* const*)frame->data, frame->linesize,
                                0, h, padded_data, padded_linesize);

                            uint8_t* pixels = malloc((size_t)w * h * 4);
                            if (pixels) {
                                for (int y = 0; y < h; y++) {
                                    memcpy(pixels + (size_t)y * w * 4,
                                        padded_data[0] + (size_t)y * padded_linesize[0],
                                        (size_t)w * 4);
                                }
                                out->pixels = pixels;
                                out->width = w;
                                out->height = h;
                                out->source_width = w;
                                out->source_height = h;
                                out->path = PATH_FULL_DECODE;
                                result = 1;
                            }
                            av_freep(&padded_data[0]);
                        }
                        sws_freeContext(sws);
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return result;
}
