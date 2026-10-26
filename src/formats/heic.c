#include "../third_party/stb_image.h"
#include "exif_apply.h"
#include "formats.h"
#include "isobmff.h"
#include "tiff_ifd.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libheif/heif.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Tier 2: locate the 'Exif' item via meta/iinf/iloc, read its TIFF
 * blob (which for HEIC/camera files usually carries the same IFD1
 * embedded-JPEG-thumbnail convention as regular EXIF-JPEG/TIFF-RAW), and
 * decode that tiny JPEG instead of the full-resolution HEVC image. ---- */

static uint64_t read_uint_be(const uint8_t* p, int nbytes)
{
    uint64_t v = 0;
    for (int i = 0; i < nbytes; i++)
        v = (v << 8) | p[i];
    return v;
}

typedef struct {
    uint32_t item_id;
    int found;
} infe_find_ctx_t;

/* Walks the iinf box's child 'infe' boxes looking for item_type=="Exif",
 * version 2/3 only (what real-world HEIF encoders emit). */
static int find_infe_cb(const isobmff_box_t* box, void* vctx)
{
    infe_find_ctx_t* ctx = (infe_find_ctx_t*)vctx;
    if (box->type != ISOBMFF_FOURCC('i', 'n', 'f', 'e'))
        return 1;
    if (box->body_len < 4)
        return 1;
    uint8_t version = box->body[0];
    if (version < 2)
        return 1; /* legacy infe layout not supported */

    size_t off = 4;
    uint32_t item_id;
    if (version == 2) {
        if (box->body_len < off + 2)
            return 1;
        item_id = (uint32_t)read_uint_be(box->body + off, 2);
        off += 2;
    } else {
        if (box->body_len < off + 4)
            return 1;
        item_id = (uint32_t)read_uint_be(box->body + off, 4);
        off += 4;
    }
    off += 2; /* item_protection_index */
    if (box->body_len < off + 4)
        return 1;
    uint32_t item_type = (uint32_t)read_uint_be(box->body + off, 4);

    if (item_type == ISOBMFF_FOURCC('E', 'x', 'i', 'f')) {
        ctx->item_id = item_id;
        ctx->found = 1;
        return 0;
    }
    return 1;
}

/* Finds the Exif item's absolute byte range in the file via the iloc box.
 * Only handles construction_method 0 (file offset) and takes the first
 * extent, which covers the overwhelming common case. */
static int find_iloc_range(const isobmff_box_t* iloc, uint32_t want_item_id,
    uint64_t* out_offset, uint64_t* out_length)
{
    const uint8_t* b = iloc->body;
    size_t len = iloc->body_len;
    if (len < 4)
        return 0;
    uint8_t version = b[0];
    size_t off = 4;

    if (len < off + 2)
        return 0;
    int offset_size = b[off] >> 4;
    int length_size = b[off] & 0xF;
    off += 1;
    int base_offset_size = b[off] >> 4;
    int index_size = b[off] & 0xF;
    off += 1;

    int item_count_size = (version < 2) ? 2 : 4;
    if (len < off + (size_t)item_count_size)
        return 0;
    uint32_t item_count = (uint32_t)read_uint_be(b + off, item_count_size);
    off += item_count_size;

    int item_id_size = (version < 2) ? 2 : 4;

    for (uint32_t i = 0; i < item_count; i++) {
        if (len < off + (size_t)item_id_size)
            return 0;
        uint32_t item_id = (uint32_t)read_uint_be(b + off, item_id_size);
        off += item_id_size;

        int construction_method = 0;
        if (version == 1 || version == 2) {
            if (len < off + 2)
                return 0;
            construction_method = (int)(read_uint_be(b + off, 2) & 0xF);
            off += 2;
        }

        if (len < off + 2)
            return 0;
        off += 2; /* data_reference_index */

        if (len < off + (size_t)base_offset_size)
            return 0;
        uint64_t base_offset = base_offset_size ? read_uint_be(b + off, base_offset_size) : 0;
        off += base_offset_size;

        if (len < off + 2)
            return 0;
        uint32_t extent_count = (uint32_t)read_uint_be(b + off, 2);
        off += 2;

        uint64_t first_extent_offset = 0, first_extent_length = 0;
        for (uint32_t e = 0; e < extent_count; e++) {
            if ((version == 1 || version == 2) && index_size > 0) {
                if (len < off + (size_t)index_size)
                    return 0;
                off += index_size;
            }
            if (len < off + (size_t)offset_size)
                return 0;
            uint64_t ext_off = offset_size ? read_uint_be(b + off, offset_size) : 0;
            off += offset_size;
            if (len < off + (size_t)length_size)
                return 0;
            uint64_t ext_len = length_size ? read_uint_be(b + off, length_size) : 0;
            off += length_size;

            if (e == 0) {
                first_extent_offset = ext_off;
                first_extent_length = ext_len;
            }
        }

        if (item_id == want_item_id) {
            if (construction_method != 0)
                return 0; /* idat/item construction not supported */
            *out_offset = base_offset + first_extent_offset;
            *out_length = first_extent_length;
            return 1;
        }
    }
    return 0;
}

/* Attempts the tier-2 embedded-Exif-thumbnail path. Returns 1 on success
 * (out filled), 0 if not available/parseable (caller should fall back to
 * tier 3). */
static int try_embedded_exif_thumbnail(const char* path, image_t* out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) {
        fclose(f);
        return 0;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t* filebuf = malloc((size_t)fsize);
    if (!filebuf) {
        fclose(f);
        return 0;
    }
    size_t rd = fread(filebuf, 1, (size_t)fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) {
        free(filebuf);
        return 0;
    }

    int result = 0;
    isobmff_box_t meta_box;
    if (!isobmff_find_box(filebuf, (size_t)fsize, ISOBMFF_FOURCC('m', 'e', 't', 'a'), &meta_box))
        goto done;
    if (meta_box.body_len < 4)
        goto done;
    const uint8_t* meta_children = meta_box.body + 4; /* skip FullBox version+flags */
    size_t meta_children_len = meta_box.body_len - 4;

    isobmff_box_t iinf_box, iloc_box;
    if (!isobmff_find_box(meta_children, meta_children_len, ISOBMFF_FOURCC('i', 'i', 'n', 'f'), &iinf_box))
        goto done;
    if (!isobmff_find_box(meta_children, meta_children_len, ISOBMFF_FOURCC('i', 'l', 'o', 'c'), &iloc_box))
        goto done;

    if (iinf_box.body_len < 4)
        goto done;
    uint8_t iinf_version = iinf_box.body[0];
    size_t iinf_hdr = 4 + (iinf_version == 0 ? 2 : 4);
    if (iinf_box.body_len < iinf_hdr)
        goto done;

    infe_find_ctx_t ctx = { 0, 0 };
    isobmff_foreach_box(iinf_box.body + iinf_hdr, iinf_box.body_len - iinf_hdr, find_infe_cb, &ctx);
    if (!ctx.found)
        goto done;

    uint64_t exif_off, exif_len;
    if (!find_iloc_range(&iloc_box, ctx.item_id, &exif_off, &exif_len))
        goto done;
    if (exif_off + exif_len > (uint64_t)fsize || exif_len < 10)
        goto done;

    /* Exif item payload: 4-byte big-endian "TIFF header offset" field,
     * then (optionally) "Exif\0\0", then the TIFF blob at that offset. */
    const uint8_t* item_data = filebuf + exif_off;
    uint32_t tiff_header_offset = (uint32_t)read_uint_be(item_data, 4);
    if (4 + (uint64_t)tiff_header_offset >= exif_len)
        goto done;
    const uint8_t* tiff_data = item_data + 4 + tiff_header_offset;
    size_t tiff_len = exif_len - 4 - tiff_header_offset;

    uint32_t jpg_off, jpg_len;
    if (!tiff_find_largest_jpeg(tiff_data, tiff_len, &jpg_off, &jpg_len))
        goto done;
    if ((size_t)jpg_off + jpg_len > tiff_len)
        goto done;

    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(tiff_data + jpg_off, (int)jpg_len, &w, &h, &channels, 4);
    if (!pixels)
        goto done;

    out->pixels = pixels;
    out->width = w;
    out->height = h;
    out->path = PATH_EMBEDDED;
    exif_apply_orientation(out, tiff_read_orientation(tiff_data, tiff_len));
    result = 1;

done:
    free(filebuf);
    return result;
}

/* ---- Tier 3: full decode, hardware-accelerated with software fallback ---- */

/* Preferred hw device type. Pick the one matching your deployment
 * target - VAAPI (Linux/Intel/AMD), VIDEOTOOLBOX (macOS/iOS),
 * D3D11VA/DXVA2 (Windows), CUDA (NVIDIA via NVDEC), or QSV. */
#define HEIC_HWACCEL_DEVICE_TYPE AV_HWDEVICE_TYPE_VAAPI

/* One hw device context per worker thread, lazily created on first use
 * and reused for every file that thread processes. Creating this per
 * file was measured to cost 10-70ms (worse under concurrency, pointing
 * to driver/kernel-level contention on device/render-node setup) -
 * amortizing it over the thread's whole lifetime removes it from the
 * per-file critical path entirely, with no signature changes needed on
 * decode_heic / decode_heic_full. NULL after the first attempt means hw
 * is unavailable on this thread; callers fall back to the sw path and
 * do not retry hw per file. */
static _Thread_local AVBufferRef* tls_heic_hw_device_ctx = NULL;
static _Thread_local int tls_heic_hw_tried = 0;

static AVBufferRef* get_thread_hw_device_ctx(void)
{
    if (!tls_heic_hw_tried) {
        tls_heic_hw_tried = 1;
        if (av_hwdevice_ctx_create(&tls_heic_hw_device_ctx, HEIC_HWACCEL_DEVICE_TYPE, NULL, NULL, 0) < 0)
            tls_heic_hw_device_ctx = NULL;
    }
    return tls_heic_hw_device_ctx;
}

/* Original libheif-based full decoder. Kept as the fallback for when
 * hw init/decode fails or isn't available, and for codecs/containers
 * the hw path doesn't handle. */
static int decode_heic_full_sw(const char* path, image_t* out)
{
    struct heif_context* ctx = heif_context_alloc();
    if (!ctx)
        return -1;

    struct heif_error err = heif_context_read_from_file(ctx, path, NULL);
    if (err.code != heif_error_Ok) {
        heif_context_free(ctx);
        return -1;
    }

    struct heif_image_handle* handle = NULL;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) {
        heif_context_free(ctx);
        return -1;
    }

    struct heif_image* img = NULL;
    err = heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, NULL);
    if (err.code != heif_error_Ok || !img) {
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return -1;
    }

    int stride;
    const uint8_t* plane = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    int w = heif_image_get_width(img, heif_channel_interleaved);
    int h = heif_image_get_height(img, heif_channel_interleaved);
    if (!plane || w <= 0 || h <= 0) {
        heif_image_release(img);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return -1;
    }

    uint8_t* pixels = malloc((size_t)w * h * 4);
    if (!pixels) {
        heif_image_release(img);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        memcpy(pixels + (size_t)y * w * 4, plane + (size_t)y * stride, (size_t)w * 4);
    }

    out->pixels = pixels;
    out->width = w;
    out->height = h;
    out->source_width = w;
    out->source_height = h;
    out->path = PATH_FULL_DECODE;

    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return 1;
}

/* Queries the real container dimensions (the actual photo's resolution)
 * without decoding any pixel data. Used so even a cheap preview path can
 * report the true original resolution for Thumb::Image::Width/Height. */
static int get_heic_container_dimensions(const char* path, int* w, int* h)
{
    struct heif_context* ctx = heif_context_alloc();
    if (!ctx)
        return 0;

    struct heif_error err = heif_context_read_from_file(ctx, path, NULL);
    if (err.code != heif_error_Ok) {
        heif_context_free(ctx);
        return 0;
    }

    struct heif_image_handle* handle = NULL;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) {
        heif_context_free(ctx);
        return 0;
    }

    int width = heif_image_handle_get_width(handle);
    int height = heif_image_handle_get_height(handle);

    heif_image_handle_release(handle);
    heif_context_free(ctx);

    if (width <= 0 || height <= 0)
        return 0;
    *w = width;
    *h = height;
    return 1;
}

typedef struct {
    enum AVPixelFormat hw_pix_fmt;
} hw_decode_ctx_t;

static enum AVPixelFormat get_hw_format(AVCodecContext* codec_ctx,
    const enum AVPixelFormat* pix_fmts)
{
    hw_decode_ctx_t* hwctx = (hw_decode_ctx_t*)codec_ctx->opaque;
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == hwctx->hw_pix_fmt)
            return *p;
    }
    return AV_PIX_FMT_NONE; /* decoder will fail to open; caller falls back */
}

/* Finds the hw pixel format the decoder advertises for the given
 * device type (e.g. AV_PIX_FMT_VAAPI), needed to set up get_format(). */
static enum AVPixelFormat find_hw_pix_fmt(const AVCodec* codec,
    enum AVHWDeviceType device_type)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (!cfg)
            return AV_PIX_FMT_NONE;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == device_type) {
            return cfg->pix_fmt;
        }
    }
}

/* Non-tile-grid hw path: single AVStream, single HEVC frame (e.g.
 * screenshots or non-grid captures). fmt_ctx is owned by the caller. */
static int decode_heic_full_hw(AVFormatContext* fmt_ctx, AVBufferRef* hw_device_ctx, image_t* out)
{
    AVCodecContext* codec_ctx = NULL;
    AVPacket* pkt = NULL;
    AVFrame* frame = NULL;
    AVFrame* sw_frame = NULL;
    struct SwsContext* sws = NULL;
    uint8_t* padded_data[4] = { NULL, NULL, NULL, NULL };
    int padded_linesize[4] = { 0, 0, 0, 0 };
    int result = -1;

    if (!hw_device_ctx)
        return -1;

    int stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream_idx < 0)
        goto cleanup;
    AVStream* stream = fmt_ctx->streams[stream_idx];

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        goto cleanup;

    enum AVPixelFormat hw_pix_fmt = find_hw_pix_fmt(codec, HEIC_HWACCEL_DEVICE_TYPE);
    if (hw_pix_fmt == AV_PIX_FMT_NONE)
        goto cleanup; /* no hw path for this codec/device */

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
        goto cleanup;
    if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0)
        goto cleanup;

    hw_decode_ctx_t hwctx = { .hw_pix_fmt = hw_pix_fmt };
    codec_ctx->opaque = &hwctx;
    codec_ctx->get_format = get_hw_format;
    codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    if (avcodec_open2(codec_ctx, codec, NULL) < 0)
        goto cleanup;

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    sw_frame = av_frame_alloc();
    if (!pkt || !frame || !sw_frame)
        goto cleanup;

    while (result != 1 && av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                if (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    AVFrame* src = frame;
                    if (frame->format == hw_pix_fmt) {
                        if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                            av_packet_unref(pkt);
                            break;
                        }
                        src = sw_frame;
                    }

                    int w = src->width, h = src->height;
                    sws = sws_getContext(
                        w, h, (enum AVPixelFormat)src->format,
                        w, h, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, NULL, NULL, NULL);
                    if (sws) {
                        if (av_image_alloc(padded_data, padded_linesize,
                                w, h, AV_PIX_FMT_RGBA, 32)
                            >= 0) {
                            sws_scale(sws, (const uint8_t* const*)src->data, src->linesize,
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
                        sws = NULL;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

cleanup:
    if (pkt)
        av_packet_free(&pkt);
    if (frame)
        av_frame_free(&frame);
    if (sw_frame)
        av_frame_free(&sw_frame);
    if (codec_ctx)
        avcodec_free_context(&codec_ctx);
    return result;
}

static int heic_find_tile_grid(AVFormatContext* fmt_ctx,
    const AVStreamGroupTileGrid** out_grid,
    AVStreamGroup** out_group)
{
    for (unsigned i = 0; i < fmt_ctx->nb_stream_groups; i++) {
        AVStreamGroup* g = fmt_ctx->stream_groups[i];
        if (g->type == AV_STREAM_GROUP_PARAMS_TILE_GRID) {
            *out_grid = g->params.tile_grid;
            *out_group = g;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    AVPacket** pkts;
    int count;
    int cap;
} tile_pkt_buf_t;

static int tile_pkt_buf_push(tile_pkt_buf_t* buf, AVPacket* pkt)
{
    if (buf->count == buf->cap) {
        int new_cap = buf->cap ? buf->cap * 2 : 4;
        AVPacket** grown = realloc(buf->pkts, sizeof(AVPacket*) * new_cap);
        if (!grown)
            return 0;
        buf->pkts = grown;
        buf->cap = new_cap;
    }
    AVPacket* clone = av_packet_clone(pkt);
    if (!clone)
        return 0;
    buf->pkts[buf->count++] = clone;
    return 1;
}

static void tile_pkt_buf_free(tile_pkt_buf_t* buf)
{
    for (int i = 0; i < buf->count; i++)
        av_packet_free(&buf->pkts[i]);
    free(buf->pkts);
    buf->pkts = NULL;
    buf->count = buf->cap = 0;
}

#define PIPELINE_DEPTH 2

typedef struct {
    AVCodecContext* codec_ctx;
    hw_decode_ctx_t hwctx;
    struct SwsContext* sws;
    int sws_w, sws_h;
    enum AVPixelFormat sws_fmt;
    uint8_t* temp_buf;
    int has_pending;
    int tile_idx;
} decode_slot_t;

static int drain_slot(decode_slot_t* slot, const AVStreamGroupTileGrid* grid,
    enum AVPixelFormat hw_pix_fmt, uint8_t* pixels, int out_w, int out_h)
{
    if (!slot->has_pending)
        return 1;

    int dst_x = grid->offsets[slot->tile_idx].horizontal - grid->horizontal_offset;
    int dst_y = grid->offsets[slot->tile_idx].vertical - grid->vertical_offset;

    AVFrame* frame = av_frame_alloc();
    AVFrame* sw_frame = av_frame_alloc();
    int ok = 0;

    if (!frame || !sw_frame)
        goto out;

    int rc = avcodec_receive_frame(slot->codec_ctx, frame);
    if (rc == AVERROR(EAGAIN)) {
        avcodec_send_packet(slot->codec_ctx, NULL);
        rc = avcodec_receive_frame(slot->codec_ctx, frame);
    }
    if (rc < 0)
        goto out;

    AVFrame* src = frame;
    if (frame->format == hw_pix_fmt) {
        if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0)
            goto out;
        src = sw_frame;
    }

    int tw = src->width, th = src->height;
    if (dst_x >= out_w || dst_y >= out_h || dst_x + tw <= 0 || dst_y + th <= 0) {
        ok = 1;
        goto out;
    }

    if (!slot->sws || slot->sws_w != tw || slot->sws_h != th || slot->sws_fmt != src->format) {
        if (slot->sws)
            sws_freeContext(slot->sws);
        slot->sws = sws_getContext(tw, th, (enum AVPixelFormat)src->format,
            tw, th, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        slot->sws_w = tw;
        slot->sws_h = th;
        slot->sws_fmt = (enum AVPixelFormat)src->format;
    }

    if (slot->sws) {
        int clips_right = (dst_x + tw > out_w);
        int clips_left = (dst_x < 0);
        int clips_top = (dst_y < 0);
        int clips_bottom = (dst_y + th > out_h);

        if (!clips_right && !clips_left && !clips_top && !clips_bottom) {
            uint8_t* dst_data[4] = { pixels + ((size_t)dst_y * out_w + dst_x) * 4, NULL, NULL, NULL };
            int dst_linesize[4] = { out_w * 4, 0, 0, 0 };
            sws_scale(slot->sws, (const uint8_t* const*)src->data, src->linesize, 0, th, dst_data, dst_linesize);
            ok = 1;
        } else {
            if (!slot->temp_buf)
                slot->temp_buf = malloc((size_t)tw * th * 4);
            if (slot->temp_buf) {
                uint8_t* dst_data[4] = { slot->temp_buf, NULL, NULL, NULL };
                int dst_linesize[4] = { tw * 4, 0, 0, 0 };
                sws_scale(slot->sws, (const uint8_t* const*)src->data, src->linesize, 0, th, dst_data, dst_linesize);

                int start_x = dst_x < 0 ? -dst_x : 0;
                int start_y = dst_y < 0 ? -dst_y : 0;
                int copy_w = tw - start_x;
                int copy_h = th - start_y;
                if (dst_x + tw > out_w)
                    copy_w = out_w - (dst_x > 0 ? dst_x : 0);
                if (dst_y + th > out_h)
                    copy_h = out_h - (dst_y > 0 ? dst_y : 0);

                int final_dst_x = dst_x > 0 ? dst_x : 0;
                int final_dst_y = dst_y > 0 ? dst_y : 0;

                for (int y = 0; y < copy_h; y++) {
                    memcpy(pixels + ((size_t)(final_dst_y + y) * out_w + final_dst_x) * 4,
                        slot->temp_buf + ((size_t)(start_y + y) * tw + start_x) * 4,
                        (size_t)copy_w * 4);
                }
                ok = 1;
            }
        }
    }

out:
    if (frame)
        av_frame_free(&frame);
    if (sw_frame)
        av_frame_free(&sw_frame);
    slot->has_pending = 0;
    return ok;
}

static int submit_slot(decode_slot_t* slot, tile_pkt_buf_t* buf, unsigned tile_idx)
{
    for (int i = 0; i < buf->count; i++) {
        if (avcodec_send_packet(slot->codec_ctx, buf->pkts[i]) < 0)
            return 0;
    }
    slot->has_pending = 1;
    slot->tile_idx = tile_idx;
    return 1;
}

static int decode_heic_tile_grid_hw(AVFormatContext* fmt_ctx, AVBufferRef* hw_device_ctx,
    const AVStreamGroupTileGrid* grid, image_t* out)
{
    if (!hw_device_ctx || grid->nb_tiles == 0 || !grid->offsets)
        return 0;

    int out_w = grid->width;
    int out_h = grid->height;
    if (out_w <= 0 || out_h <= 0)
        return 0;

    AVStream* first_stream = fmt_ctx->streams[grid->offsets[0].idx];
    const AVCodec* codec = avcodec_find_decoder(first_stream->codecpar->codec_id);
    if (!codec)
        return 0;

    enum AVPixelFormat hw_pix_fmt = find_hw_pix_fmt(codec, HEIC_HWACCEL_DEVICE_TYPE);
    if (hw_pix_fmt == AV_PIX_FMT_NONE)
        return 0;

    int* stream_to_tile = malloc(sizeof(int) * fmt_ctx->nb_streams);
    if (!stream_to_tile)
        return 0;
    for (unsigned s = 0; s < fmt_ctx->nb_streams; s++)
        stream_to_tile[s] = -1;
    for (unsigned t = 0; t < grid->nb_tiles; t++) {
        unsigned sidx = grid->offsets[t].idx;
        if (sidx < fmt_ctx->nb_streams)
            stream_to_tile[sidx] = (int)t;
    }

    tile_pkt_buf_t* bufs = calloc(grid->nb_tiles, sizeof(tile_pkt_buf_t));
    if (!bufs) {
        free(stream_to_tile);
        return 0;
    }

    AVPacket* pkt = av_packet_alloc();
    int demux_ok = 1;
    if (!pkt) {
        demux_ok = 0;
    } else {
        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            int tile_i = (pkt->stream_index >= 0 && pkt->stream_index < (int)fmt_ctx->nb_streams)
                ? stream_to_tile[pkt->stream_index]
                : -1;
            if (tile_i >= 0) {
                if (!tile_pkt_buf_push(&bufs[tile_i], pkt)) {
                    demux_ok = 0;
                    av_packet_unref(pkt);
                    break;
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    free(stream_to_tile);

    if (!demux_ok) {
        for (unsigned t = 0; t < grid->nb_tiles; t++)
            tile_pkt_buf_free(&bufs[t]);
        free(bufs);
        return 0;
    }

    uint8_t* pixels = malloc((size_t)out_w * out_h * 4);
    if (!pixels) {
        for (unsigned t = 0; t < grid->nb_tiles; t++)
            tile_pkt_buf_free(&bufs[t]);
        free(bufs);
        return 0;
    }

    uint32_t bg_color;
    memcpy(&bg_color, grid->background, 4);
    if (bg_color == 0) {
        memset(pixels, 0, (size_t)out_w * out_h * 4);
    } else {
        uint32_t* p = (uint32_t*)pixels;
        for (size_t i = 0; i < (size_t)out_w * out_h; i++)
            p[i] = bg_color;
    }

    decode_slot_t slots[PIPELINE_DEPTH] = { 0 };
    int open_ok = 1;
    for (int i = 0; i < PIPELINE_DEPTH; i++) {
        slots[i].codec_ctx = avcodec_alloc_context3(codec);
        if (!slots[i].codec_ctx || avcodec_parameters_to_context(slots[i].codec_ctx, first_stream->codecpar) < 0) {
            open_ok = 0;
            break;
        }
        slots[i].hwctx.hw_pix_fmt = hw_pix_fmt;
        slots[i].codec_ctx->opaque = &slots[i].hwctx;
        slots[i].codec_ctx->get_format = get_hw_format;
        slots[i].codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        if (avcodec_open2(slots[i].codec_ctx, codec, NULL) < 0) {
            open_ok = 0;
            break;
        }
    }

    int all_ok = open_ok;
    if (open_ok) {
        for (unsigned i = 0; i < grid->nb_tiles && all_ok; i++) {
            unsigned sidx = grid->offsets[i].idx;
            if (sidx >= fmt_ctx->nb_streams || bufs[i].count == 0) {
                all_ok = 0;
                break;
            }

            decode_slot_t* slot = &slots[i % PIPELINE_DEPTH];

            if (!drain_slot(slot, grid, hw_pix_fmt, pixels, out_w, out_h)) {
                all_ok = 0;
                break;
            }
            if (!submit_slot(slot, &bufs[i], i)) {
                all_ok = 0;
                break;
            }
        }
        if (all_ok) {
            for (int i = 0; i < PIPELINE_DEPTH; i++) {
                if (!drain_slot(&slots[i], grid, hw_pix_fmt, pixels, out_w, out_h)) {
                    all_ok = 0;
                    break;
                }
            }
        }
    }

    for (int i = 0; i < PIPELINE_DEPTH; i++) {
        if (slots[i].codec_ctx)
            avcodec_free_context(&slots[i].codec_ctx);
        if (slots[i].sws)
            sws_freeContext(slots[i].sws);
        if (slots[i].temp_buf)
            free(slots[i].temp_buf);
    }

    for (unsigned t = 0; t < grid->nb_tiles; t++)
        tile_pkt_buf_free(&bufs[t]);
    free(bufs);

    if (!all_ok) {
        free(pixels);
        return 0;
    }

    out->pixels = pixels;
    out->width = out_w;
    out->height = out_h;
    out->source_width = out_w;
    out->source_height = out_h;
    out->path = PATH_FULL_DECODE;
    return 1;
}

/* Dispatcher: opens the file once, detects tile-grid vs single-image
 * HEIC, tries the appropriate hw path using this thread's shared
 * hw_device_ctx, and falls back to the libheif software decoder on any
 * failure or when hw is unavailable on this thread. */
static int decode_heic_full(const char* path, image_t* out)
{
    AVFormatContext* fmt_ctx = NULL;
    int have_fmt = (avformat_open_input(&fmt_ctx, path, NULL, NULL) == 0 && avformat_find_stream_info(fmt_ctx, NULL) >= 0);

    if (have_fmt) {
        const AVStreamGroupTileGrid* grid = NULL;
        AVStreamGroup* group = NULL;

        if (heic_find_tile_grid(fmt_ctx, &grid, &group)) {
            AVBufferRef* hw_device_ctx = get_thread_hw_device_ctx();

            if (hw_device_ctx) {
                int rc = decode_heic_tile_grid_hw(fmt_ctx, hw_device_ctx, grid, out);
                avformat_close_input(&fmt_ctx);
                if (rc == 1)
                    return 1;
                return decode_heic_full_sw(path, out);
            }
            avformat_close_input(&fmt_ctx);
            return decode_heic_full_sw(path, out);
        }

        /* Non-tile-grid HEIC (single-image capture). */
        AVBufferRef* hw_device_ctx = get_thread_hw_device_ctx();

        if (hw_device_ctx) {
            int rc = decode_heic_full_hw(fmt_ctx, hw_device_ctx, out);
            avformat_close_input(&fmt_ctx);
            if (rc == 1) {
                int true_w, true_h;
                if (get_heic_container_dimensions(path, &true_w, &true_h) && (out->width != true_w || out->height != true_h)) {
                    image_free(out);
                } else {
                    return 1;
                }
            }
            return decode_heic_full_sw(path, out);
        }
        avformat_close_input(&fmt_ctx);
        return decode_heic_full_sw(path, out);
    }

    if (fmt_ctx)
        avformat_close_input(&fmt_ctx);
    return decode_heic_full_sw(path, out);
}

int decode_heic(const char* path, image_t* out, int target_box)
{
    if (try_embedded_exif_thumbnail(path, out)) {
        if (out->width >= target_box || out->height >= target_box) {
            int true_w, true_h;
            if (get_heic_container_dimensions(path, &true_w, &true_h)) {
                out->source_width = true_w;
                out->source_height = true_h;
            } else {
                out->source_width = out->width;
                out->source_height = out->height;
            }
            return 1;
        }
        image_free(out);
    }
    return decode_heic_full(path, out);
}
