#include "exif_apply.h"
#include "formats.h"
#include "tiff_ifd.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

struct jpeg_error_ctx {
    struct jpeg_error_mgr pub;
    jmp_buf escape;
};

/* libjpeg's default error_exit() calls exit(), which would take the whole
 * process (and every other worker thread) down over a single corrupt
 * JPEG. Override it to longjmp back into decode_jpeg instead, which then
 * cleans up and returns -1 like any other decode error. */
static void jpeg_error_longjmp(j_common_ptr cinfo)
{
    struct jpeg_error_ctx* err = (struct jpeg_error_ctx*)cinfo->err;
    longjmp(err->escape, 1);
}

/* Silences libjpeg console spam for recoverable warnings (like malformed SOS markers) */
static void jpeg_output_message_noop(j_common_ptr cinfo)
{
    (void)cinfo;
}

/* Orientations 5-8 (transpose/rotate90/transverse/rotate270) swap the
 * displayed width and height - matches the logic in exif_apply.c. */
static int orientation_swaps_dims(int orientation)
{
    return orientation >= 5 && orientation <= 8;
}

int decode_jpeg(const char* path, image_t* out, int target_box)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_ctx jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_longjmp;
    jerr.pub.output_message = jpeg_output_message_noop;

    uint8_t* pixels = NULL;

    if (setjmp(jerr.escape)) {
        /* Landed here via longjmp from jpeg_error_longjmp - some libjpeg
         * call above failed. Clean up whatever was allocated and bail. */
        jpeg_destroy_decompress(&cinfo);
        if (pixels)
            free(pixels);
        fclose(f);
        return -1;
    }

    jpeg_create_decompress(&cinfo);

    /* Let libjpeg stream directly from the disk block-by-block.
     * Eliminates the massive whole-file RAM allocation! */
    jpeg_stdio_src(&cinfo, f);

    /* Tell libjpeg to save APP1 (EXIF) markers in memory while it reads the header */
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return -1;
    }

    /* Extract Orientation from the saved APP1 marker list */
    int orientation = 1;
    jpeg_saved_marker_ptr marker = cinfo.marker_list;
    while (marker) {
        if (marker->marker == JPEG_APP0 + 1 && marker->data_length >= 6) {
            if (memcmp(marker->data, "Exif\0\0", 6) == 0) {
                orientation = tiff_read_orientation(marker->data + 6, marker->data_length - 6);
                break;
            }
        }
        marker = marker->next;
    }

    /* True original dimensions, unaffected by the scale_num/scale_denom
     * we're about to set - captured now for Thumb::Image::Width/Height */
    int true_w = (int)cinfo.image_width;
    int true_h = (int)cinfo.image_height;

    /* Use libjpeg's built-in DCT-domain scaling to decode directly at a
     * smaller size when the source is much bigger than what we need. */
    unsigned int denom = 1;
    static const unsigned int candidates[] = { 8, 4, 2 };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        unsigned int d = candidates[i];
        int scaled_w = (true_w + (int)d - 1) / (int)d;
        int scaled_h = (true_h + (int)d - 1) / (int)d;
        if (scaled_w >= target_box || scaled_h >= target_box) {
            denom = d;
            break;
        }
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;

    /* IFAST trades a small amount of precision for a meaningful speedup */
    cinfo.dct_method = JDCT_FASTEST;
    cinfo.out_color_space = JCS_EXT_RGBA;

    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    int row_stride = w * 4;

    pixels = malloc((size_t)row_stride * h);
    if (!pixels) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return -1;
    }

    /* ZERO-COPY LOOP: Decode straight into the final pixel buffer! */
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char* rows[1] = { pixels + (size_t)cinfo.output_scanline * row_stride };
        jpeg_read_scanlines(&cinfo, rows, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);

    out->pixels = pixels;
    out->width = w;
    out->height = h;
    out->path = PATH_DIRECT;

    exif_apply_orientation(out, orientation);

    /* source dims must reflect the final, post-rotation TRUE original size */
    if (orientation_swaps_dims(orientation)) {
        out->source_width = true_h;
        out->source_height = true_w;
    } else {
        out->source_width = true_w;
        out->source_height = true_h;
    }
    return 1;
}
