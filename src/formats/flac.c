/* FLAC cover art: FLAC metadata blocks follow the "fLaC" magic, each
 * prefixed by a 4-byte header (1 byte: last-block flag + 7-bit block
 * type, 3 bytes: big-endian block length). Block type 6 is PICTURE,
 * structured per the FLAC spec (all fields big-endian):
 *   picture_type(4) mime_len(4) mime[mime_len] desc_len(4) desc[desc_len]
 *   width(4) height(4) depth(4) colors(4) data_len(4) data[data_len]
 * Returns 0 (not an error) if there's no PICTURE block - very common. */
#include "formats.h"
#include "../third_party/stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int decode_flac(const char *path, image_t *out, int target_box) {
    (void)target_box;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) {
        fclose(f);
        return -1; /* .flac extension but not actually a FLAC stream */
    }

    int result = 0;
    for (;;) {
        uint8_t block_header[4];
        if (fread(block_header, 1, 4, f) != 4) break;

        int is_last = (block_header[0] & 0x80) != 0;
        int block_type = block_header[0] & 0x7F;
        uint32_t block_len = ((uint32_t)block_header[1] << 16) |
                              ((uint32_t)block_header[2] << 8) | block_header[3];

        if (block_type == 6) { /* PICTURE */
            uint8_t *block = malloc(block_len);
            if (!block) { fclose(f); return -1; }
            if (fread(block, 1, block_len, f) != block_len) { free(block); break; }

            size_t off = 0;
            if (off + 4 <= block_len) {
                off += 4; /* picture_type */
                if (off + 4 <= block_len) {
                    uint32_t mime_len = read_u32be(block + off);
                    off += 4 + mime_len;
                    if (off + 4 <= block_len) {
                        uint32_t desc_len = read_u32be(block + off);
                        off += 4 + desc_len;
                        off += 16; /* width, height, depth, colors */
                        if (off + 4 <= block_len) {
                            uint32_t data_len = read_u32be(block + off);
                            off += 4;
                            if (off + data_len <= block_len) {
                                int w, h, channels;
                                unsigned char *pixels = stbi_load_from_memory(
                                    block + off, (int)data_len, &w, &h, &channels, 4);
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
                        }
                    }
                }
            }
            free(block);
            if (result) break;
        } else {
            if (fseek(f, block_len, SEEK_CUR) != 0) break;
        }

        if (is_last) break;
    }

    fclose(f);
    return result;
}
