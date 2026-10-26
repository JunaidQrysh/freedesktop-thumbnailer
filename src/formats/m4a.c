/* M4A cover art lives in the iTunes-style metadata atom chain:
 * moov -> udta -> meta (a FullBox, 4-byte version+flags prefix) -> ilst
 * -> covr -> data. The 'data' box itself has an 8-byte payload header
 * (4-byte type indicator + 4-byte locale/reserved) before the raw image
 * bytes. */
#include "formats.h"
#include "isobmff.h"
#include "../third_party/stb_image.h"
#include <stdio.h>
#include <stdlib.h>

int decode_m4a(const char *path, image_t *out, int target_box) {
    (void)target_box;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) { free(buf); return -1; }

    int result = 0;
    isobmff_box_t moov, udta, meta, ilst, covr, data;

    if (!isobmff_find_box(buf, (size_t)fsize, ISOBMFF_FOURCC('m','o','o','v'), &moov)) goto done;
    if (!isobmff_find_box(moov.body, moov.body_len, ISOBMFF_FOURCC('u','d','t','a'), &udta)) goto done;
    if (!isobmff_find_box(udta.body, udta.body_len, ISOBMFF_FOURCC('m','e','t','a'), &meta)) goto done;
    if (meta.body_len < 4) goto done; /* meta is a FullBox: skip version+flags */
    if (!isobmff_find_box(meta.body + 4, meta.body_len - 4, ISOBMFF_FOURCC('i','l','s','t'), &ilst)) goto done;
    if (!isobmff_find_box(ilst.body, ilst.body_len, ISOBMFF_FOURCC('c','o','v','r'), &covr)) goto done;
    if (!isobmff_find_box(covr.body, covr.body_len, ISOBMFF_FOURCC('d','a','t','a'), &data)) goto done;
    if (data.body_len < 8) goto done;

    {
        const uint8_t *img_bytes = data.body + 8;
        size_t img_len = data.body_len - 8;
        int w, h, channels;
        unsigned char *pixels = stbi_load_from_memory(img_bytes, (int)img_len, &w, &h, &channels, 4);
        if (pixels) {
            out->pixels = pixels;
            out->width = w;
            out->height = h;
            out->source_width = w;
            out->source_height = h;
            out->path = PATH_EMBEDDED;
            result = 1;
        }
    }

done:
    free(buf);
    return result;
}
