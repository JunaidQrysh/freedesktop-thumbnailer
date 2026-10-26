/* MP3 cover art: parses the ID3v2 tag at the start of the file for an
 * APIC frame (ID3v2.3/2.4) or PIC frame (ID3v2.2, 3-char frame IDs), and
 * decodes the embedded image via stb_image. Returns 0 (no image, not an
 * error) if there's no ID3v2 tag or no picture frame in it - that's a
 * completely normal, common case for MP3 files. */
#include "formats.h"
#include "../third_party/stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t synchsafe_to_u32(const uint8_t b[4]) {
    return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) |
           ((uint32_t)(b[2] & 0x7F) << 7) | (uint32_t)(b[3] & 0x7F);
}

static uint32_t plain_be_to_u32(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/* Skips a text field encoded per `encoding` byte (0/3 = single-byte
 * charset -> single NUL terminator; 1/2 = UTF-16 variants -> double NUL,
 * 2-byte aligned). Returns the offset just past the terminator, or
 * `limit` if no terminator was found (caller should treat as malformed). */
static size_t skip_encoded_string(const uint8_t *p, size_t off, size_t limit, uint8_t encoding) {
    if (encoding == 1 || encoding == 2) {
        size_t i = off;
        while (i + 1 < limit) {
            if (p[i] == 0 && p[i + 1] == 0) return i + 2;
            i += 2;
        }
        return limit;
    } else {
        size_t i = off;
        while (i < limit) {
            if (p[i] == 0) return i + 1;
            i++;
        }
        return limit;
    }
}

/* Tries to decode an image starting at data[off], length `len`. Returns 1
 * on success (out filled), 0 otherwise. */
static int try_decode_at(const uint8_t *data, size_t len, image_t *out) {
    if (len == 0) return 0;
    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 4);
    if (!pixels) return 0;
    out->pixels = pixels;
    out->width = w;
    out->height = h;
    out->source_width = w;
    out->source_height = h;
    out->path = PATH_EMBEDDED;
    return 1;
}

int decode_mp3(const char *path, image_t *out, int target_box) {
    (void)target_box;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t header[10];
    if (fread(header, 1, 10, f) != 10 || memcmp(header, "ID3", 3) != 0) {
        fclose(f);
        return 0; /* no ID3v2 tag at all - not an error, just no art */
    }

    uint8_t major_version = header[3];
    uint8_t flags = header[5];
    uint32_t tag_size = synchsafe_to_u32(header + 6); /* excludes the 10-byte header */

    uint8_t *tag = malloc(tag_size);
    if (!tag) { fclose(f); return -1; }
    size_t rd = fread(tag, 1, tag_size, f);
    fclose(f);
    if (rd != tag_size) { free(tag); return -1; }

    size_t pos = 0;
    /* extended header, if present, sits right after the main header */
    if (flags & 0x40) {
        if (tag_size < 4) { free(tag); return 0; }
        uint32_t ext_size = (major_version >= 4) ? synchsafe_to_u32(tag) : plain_be_to_u32(tag);
        /* v2.3 ext header size field EXCLUDES itself, v2.4 INCLUDES itself;
         * treat both leniently by just skipping the reported size plus
         * the 4-byte size field for v2.3 */
        pos = (major_version >= 4) ? ext_size : (4 + ext_size);
        if (pos > tag_size) pos = tag_size;
    }

    int result = 0;
    while (pos + 6 <= tag_size) {
        char frame_id[5] = {0};
        uint32_t frame_size;
        size_t frame_header_len;

        if (major_version == 2) {
            /* 3-char ID, 3-byte plain size, 6-byte header total */
            if (pos + 6 > tag_size) break;
            memcpy(frame_id, tag + pos, 3);
            frame_size = ((uint32_t)tag[pos + 3] << 16) | ((uint32_t)tag[pos + 4] << 8) | tag[pos + 5];
            frame_header_len = 6;
        } else {
            if (pos + 10 > tag_size) break;
            memcpy(frame_id, tag + pos, 4);
            frame_size = (major_version >= 4) ? synchsafe_to_u32(tag + pos + 4)
                                               : plain_be_to_u32(tag + pos + 4);
            frame_header_len = 10;
        }

        if (frame_id[0] == 0) break; /* padding reached */
        if (pos + frame_header_len + frame_size > tag_size) break; /* malformed/truncated */

        int is_picture_frame = (major_version == 2) ? (strcmp(frame_id, "PIC") == 0)
                                                      : (strcmp(frame_id, "APIC") == 0);

        if (is_picture_frame) {
            const uint8_t *fdata = tag + pos + frame_header_len;
            size_t flen = frame_size;
            if (flen < 2) { pos += frame_header_len + frame_size; continue; }

            uint8_t encoding = fdata[0];
            size_t off = 1;

            if (major_version == 2) {
                off += 3; /* 3-char image format code, fixed width, no terminator */
            } else {
                /* MIME type: always Latin1, single-NUL terminated regardless of encoding */
                while (off < flen && fdata[off] != 0) off++;
                off += 1;
            }
            if (off >= flen) { pos += frame_header_len + frame_size; continue; }

            off += 1; /* picture type byte */
            if (off > flen) { pos += frame_header_len + frame_size; continue; }

            off = skip_encoded_string(fdata, off, flen, encoding); /* description */
            if (off >= flen) { pos += frame_header_len + frame_size; continue; }

            if (try_decode_at(fdata + off, flen - off, out)) {
                result = 1;
                break;
            }
        }

        pos += frame_header_len + frame_size;
    }

    free(tag);
    return result;
}
