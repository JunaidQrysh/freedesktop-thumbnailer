/* CR2, NEF, ARW, and DNG are all TIFF-based containers - the whole file
 * IS a TIFF structure (CR2/DNG literally are TIFF; NEF/ARW are
 * TIFF-derived enough that the same IFD walker works). Camera RAW files
 * commonly carry one or more embedded JPEG previews (often one small
 * "thumbnail" in IFD0/IFD1 and one larger full-size preview in a
 * SubIFD) - we grab the largest one available, which gives the best
 * quality thumbnail source. Per spec, orientation correction applies
 * here too. */
#include "formats.h"
#include "tiff_ifd.h"
#include "exif_apply.h"
#include "../third_party/stb_image.h"
#include <stdio.h>
#include <stdlib.h>

int decode_raw(const char *path, image_t *out, int target_box) {
    (void)target_box;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);

    uint8_t *filebuf = malloc((size_t)fsize);
    if (!filebuf) { fclose(f); return -1; }
    size_t rd = fread(filebuf, 1, (size_t)fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) { free(filebuf); return -1; }

    uint32_t jpg_off, jpg_len;
    if (!tiff_find_largest_jpeg(filebuf, (size_t)fsize, &jpg_off, &jpg_len)) {
        free(filebuf);
        return 0; /* RAW file with no embedded preview at all - rare but not an error */
    }

    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(filebuf + jpg_off, (int)jpg_len, &w, &h, &channels, 4);
    if (!pixels) { free(filebuf); return -1; }

    out->pixels = pixels;
    out->width = w;
    out->height = h;
    out->path = PATH_EMBEDDED;
    exif_apply_orientation(out, tiff_read_orientation(filebuf, (size_t)fsize));

    /* The extracted preview is very often much smaller than the camera's
     * real sensor resolution (a small IFD0/IFD1 thumbnail next to a
     * much bigger SubIFD preview, or vice versa) - look for the largest
     * ImageWidth/ImageLength pair anywhere in the file to report the
     * true original resolution, falling back to the preview's own size
     * if no such tag is found. */
    int true_w, true_h;
    if (tiff_find_largest_dimensions(filebuf, (size_t)fsize, &true_w, &true_h) &&
        (uint64_t)true_w * true_h > (uint64_t)out->width * out->height) {
        out->source_width = true_w;
        out->source_height = true_h;
    } else {
        out->source_width = out->width;
        out->source_height = out->height;
    }

    free(filebuf);
    return 1;
}
