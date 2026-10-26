#include "decoder_registry.h"
#include "../formats/formats.h"
#include <string.h>

static const decoder_entry_t REGISTRY[] = {
    /* Tier 1: stb_image direct */
    { "jpg",  decode_jpeg },
    { "jpeg", decode_jpeg },
    { "png",  decode_stb_generic },
    { "bmp",  decode_stb_generic },
    { "gif",  decode_stb_generic },
    { "tga",  decode_stb_generic },
    { "psd",  decode_stb_generic },
    { "pnm",  decode_stb_generic },
    { "pgm",  decode_stb_generic },
    { "ppm",  decode_stb_generic },
    { "hdr",  decode_stb_generic },

    /* Tier 2 (embedded) + Tier 3 (full decode) fallback */
    { "heic", decode_heic },
    { "heif", decode_heic },

    /* Tier 2: hand-rolled container parsers + stb_image */
    { "mp3",  decode_mp3 },
    { "flac", decode_flac },
    { "ogg",  decode_ogg },
    { "opus", decode_ogg },
    { "m4a",  decode_m4a },
    { "cr2",  decode_raw },
    { "nef",  decode_raw },
    { "arw",  decode_raw },
    { "dng",  decode_raw },

    /* Tier 3: libavformat/libavcodec, single path always */
    { "mp4",  decode_video },
    { "mkv",  decode_video },
    { "webm", decode_video },
    { "mov",  decode_video },
    { "avi",  decode_video },
    { "3gp",  decode_video },
};

static const size_t REGISTRY_LEN = sizeof(REGISTRY) / sizeof(REGISTRY[0]);

decode_fn registry_find(const char *ext_lower) {
    for (size_t i = 0; i < REGISTRY_LEN; i++) {
        if (strcmp(REGISTRY[i].ext, ext_lower) == 0) return REGISTRY[i].fn;
    }
    return NULL;
}

int registry_has(const char *ext_lower) {
    return registry_find(ext_lower) != NULL;
}
