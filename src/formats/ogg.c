/* Ogg (Vorbis) and Opus cover art both live in a Vorbis-comment-style
 * header packet as a base64-encoded "METADATA_BLOCK_PICTURE" comment,
 * whose decoded bytes use the exact same structure as FLAC's PICTURE
 * metadata block. To get at that comment we have to walk Ogg's page
 * framing and reassemble packets (a packet may span multiple pages via
 * the lacing/continuation mechanism), then locate whichever packet
 * carries the comment header (Vorbis: 2nd packet, starts with
 * "\x03vorbis"; Opus: 2nd packet, starts with "OpusTags"). */
#include "formats.h"
#include "../third_party/stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t read_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* --- minimal base64 decoder --- */
static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *base64_decode(const char *s, size_t slen, size_t *out_len) {
    uint8_t *out = malloc(slen); /* over-allocated, decoded is always <= input length */
    if (!out) return NULL;
    size_t o = 0;
    int buf = 0, bits = 0;
    for (size_t i = 0; i < slen; i++) {
        int v = b64_val((unsigned char)s[i]);
        if (v < 0) continue; /* skip padding/whitespace */
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return out;
}

/* --- packet reassembly over Ogg page framing --- */

typedef struct {
    uint8_t *data;
    size_t len;
} packet_buf_t;

/* Reads up to `max_packets` logical packets from the start of the Ogg
 * stream into `packets`. Returns the number actually read (may be less
 * than max_packets at EOF/malformed stream). Caller frees packets[i].data. */
static int read_first_packets(FILE *f, packet_buf_t *packets, int max_packets) {
    int n_packets = 0;
    packet_buf_t pending = { NULL, 0 };

    while (n_packets < max_packets) {
        uint8_t page_hdr[27];
        if (fread(page_hdr, 1, 27, f) != 27) break;
        if (memcmp(page_hdr, "OggS", 4) != 0) break;

        uint8_t header_type = page_hdr[5];
        uint8_t page_segments = page_hdr[26];

        uint8_t seg_table[255];
        if (fread(seg_table, 1, page_segments, f) != page_segments) break;

        int continued = (header_type & 0x01) != 0;
        if (!continued && pending.data) {
            /* previous pending packet never got a lacing value < 255 to
             * close it out and this page doesn't continue it - discard */
            free(pending.data);
            pending.data = NULL;
            pending.len = 0;
        }

        for (int i = 0; i < page_segments && n_packets < max_packets; i++) {
            uint8_t seg_len = seg_table[i];
            uint8_t *chunk = malloc(seg_len ? seg_len : 1);
            if (seg_len > 0 && fread(chunk, 1, seg_len, f) != seg_len) { free(chunk); goto done; }

            uint8_t *newbuf = realloc(pending.data, pending.len + seg_len);
            if (!newbuf) { free(chunk); goto done; }
            pending.data = newbuf;
            memcpy(pending.data + pending.len, chunk, seg_len);
            pending.len += seg_len;
            free(chunk);

            if (seg_len < 255) {
                /* packet complete */
                packets[n_packets].data = pending.data;
                packets[n_packets].len = pending.len;
                n_packets++;
                pending.data = NULL;
                pending.len = 0;
            }
            /* else: continues into next segment/page */
        }
    }

done:
    if (pending.data) free(pending.data);
    return n_packets;
}

/* Parses the same field layout as FLAC's PICTURE metadata block (see
 * flac.c) out of already-base64-decoded bytes. */
static int decode_picture_block(const uint8_t *block, size_t block_len, image_t *out) {
    size_t off = 0;
    if (off + 4 > block_len) return 0;
    off += 4; /* picture_type */
    if (off + 4 > block_len) return 0;
    uint32_t mime_len = read_u32be(block + off);
    off += 4 + mime_len;
    if (off + 4 > block_len) return 0;
    uint32_t desc_len = read_u32be(block + off);
    off += 4 + desc_len;
    off += 16; /* width, height, depth, colors */
    if (off + 4 > block_len) return 0;
    uint32_t data_len = read_u32be(block + off);
    off += 4;
    if (off + data_len > block_len) return 0;

    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(block + off, (int)data_len, &w, &h, &channels, 4);
    if (!pixels) return 0;
    out->pixels = pixels;
    out->width = w;
    out->height = h;
    out->source_width = w;
    out->source_height = h;
    out->path = PATH_EMBEDDED;
    return 1;
}

/* Searches a Vorbis-comment-style body (vendor_len+vendor, then
 * comment_count + [len+"KEY=VALUE"]*) for METADATA_BLOCK_PICTURE. */
static int find_picture_in_comments(const uint8_t *body, size_t body_len, image_t *out) {
    if (body_len < 4) return 0;
    uint32_t vendor_len = read_u32le(body);
    size_t off = 4 + vendor_len;
    if (off + 4 > body_len) return 0;
    uint32_t comment_count = read_u32le(body + off);
    off += 4;

    for (uint32_t i = 0; i < comment_count; i++) {
        if (off + 4 > body_len) break;
        uint32_t clen = read_u32le(body + off);
        off += 4;
        if (off + clen > body_len) break;
        const char *comment = (const char *)(body + off);
        const char *prefix = "METADATA_BLOCK_PICTURE=";
        size_t prefix_len = strlen(prefix);
        if (clen > prefix_len && memcmp(comment, prefix, prefix_len) == 0) {
            size_t b64_len;
            uint8_t *decoded = base64_decode(comment + prefix_len, clen - prefix_len, &b64_len);
            if (decoded) {
                int ok = decode_picture_block(decoded, b64_len, out);
                free(decoded);
                if (ok) return 1;
            }
        }
        off += clen;
    }
    return 0;
}

int decode_ogg(const char *path, image_t *out, int target_box) {
    (void)target_box;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    packet_buf_t packets[4] = {0};
    int n = read_first_packets(f, packets, 4);
    fclose(f);

    if (n == 0) { return -1; } /* not actually an Ogg stream */

    int result = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = packets[i].data;
        size_t plen = packets[i].len;

        if (plen > 7 && p[0] == 0x03 && memcmp(p + 1, "vorbis", 6) == 0) {
            if (find_picture_in_comments(p + 7, plen - 7, out)) { result = 1; break; }
        } else if (plen > 8 && memcmp(p, "OpusTags", 8) == 0) {
            if (find_picture_in_comments(p + 8, plen - 8, out)) { result = 1; break; }
        }
    }

    for (int i = 0; i < n; i++) free(packets[i].data);
    return result;
}
