#include "formats.h"
#include "../third_party/stb_image.h"
#include <stdlib.h>

int decode_stb_generic(const char *path, image_t *out, int target_box) {
    (void)target_box;
    int w, h, channels;
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return -1;

    out->pixels = pixels; /* stb_image uses malloc, compatible with our free() */
    out->width = w;
    out->height = h;
    out->source_width = w;
    out->source_height = h;
    out->path = PATH_DIRECT;
    return 1;
}
