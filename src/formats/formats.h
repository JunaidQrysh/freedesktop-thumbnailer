#ifndef FORMATS_H
#define FORMATS_H

#include "../thumbnailer.h"

/* Tier 1: stb_image direct decode, no special per-format handling. Shared
 * by png/bmp/gif/tga/psd/pnm/pgm/ppm/hdr since stb_image auto-detects
 * format from content and none of these carry EXIF orientation. */
int decode_stb_generic(const char *path, image_t *out, int target_box);

/* Tier 1, but jpeg needs its own handler because JPEG commonly carries an
 * EXIF orientation tag that must be applied per spec. */
int decode_jpeg(const char *path, image_t *out, int target_box);

/* Tier 2 (embedded EXIF thumbnail) with Tier 3 (full libheif decode)
 * fallback - the one format with a real two-path decode. */
int decode_heic(const char *path, image_t *out, int target_box);

/* Tier 2: small hand-rolled container/tag parsers + stb_image on the
 * extracted bytes. */
int decode_mp3(const char *path, image_t *out, int target_box);   /* ID3v2 APIC */
int decode_flac(const char *path, image_t *out, int target_box);  /* PICTURE metadata block */
int decode_ogg(const char *path, image_t *out, int target_box);   /* METADATA_BLOCK_PICTURE (ogg/opus) */
int decode_m4a(const char *path, image_t *out, int target_box);   /* 'covr' atom */
int decode_raw(const char *path, image_t *out, int target_box);   /* embedded JPEG preview via TIFF IFD */

/* Tier 3: linked libavformat/libavcodec, single decode path, no
 * embedded-art pre-check. */
int decode_video(const char *path, image_t *out, int target_box);

#endif
