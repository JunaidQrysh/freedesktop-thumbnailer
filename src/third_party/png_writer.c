/* Minimal PNG encoder: hand-rolled chunk framing (length/type/data/CRC),
 * 8-bit non-interlaced RGBA, plus tEXt chunks. Uses zlib only for the
 * DEFLATE compression of IDAT and for CRC32 - not a libpng wrapper. */
#include "png_writer.h"
#include <arpa/inet.h> /* htonl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int write_chunk(FILE* f, const char type[4], const unsigned char* data, size_t len)
{
    unsigned char len_be[4];
    uint32_t len32 = (uint32_t)len;
    len_be[0] = (unsigned char)(len32 >> 24);
    len_be[1] = (unsigned char)(len32 >> 16);
    len_be[2] = (unsigned char)(len32 >> 8);
    len_be[3] = (unsigned char)(len32);
    if (fwrite(len_be, 1, 4, f) != 4)
        return -1;

    /* CRC covers type + data */
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef*)type, 4);
    if (fwrite(type, 1, 4, f) != 4)
        return -1;
    if (len > 0) {
        crc = crc32(crc, (const Bytef*)data, (uInt)len);
        if (fwrite(data, 1, len, f) != len)
            return -1;
    }

    unsigned char crc_be[4];
    uint32_t crc32v = (uint32_t)crc;
    crc_be[0] = (unsigned char)(crc32v >> 24);
    crc_be[1] = (unsigned char)(crc32v >> 16);
    crc_be[2] = (unsigned char)(crc32v >> 8);
    crc_be[3] = (unsigned char)(crc32v);
    if (fwrite(crc_be, 1, 4, f) != 4)
        return -1;
    return 0;
}

static int has_real_alpha(const uint8_t* pixels, int width, int height)
{
    size_t n = (size_t)width * height;
    for (size_t i = 0; i < n; i++) {
        if (pixels[i * 4 + 3] != 255)
            return 1;
    }
    return 0;
}

int png_write_rgba(const char* path, const uint8_t* pixels, int width,
    int height, const kv_pair_t* kv, int n_kv)
{
    if (width <= 0 || height <= 0 || !pixels)
        return -1;

    int write_alpha = has_real_alpha(pixels, width, height);
    int bpp = write_alpha ? 4 : 3;

    FILE* f = fopen(path, "wb");
    if (!f)
        return -1;

    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (fwrite(sig, 1, 8, f) != 8) {
        fclose(f);
        return -1;
    }

    /* IHDR */
    unsigned char ihdr[13];
    uint32_t w_be = (uint32_t)width, h_be = (uint32_t)height;
    ihdr[0] = (unsigned char)(w_be >> 24);
    ihdr[1] = (unsigned char)(w_be >> 16);
    ihdr[2] = (unsigned char)(w_be >> 8);
    ihdr[3] = (unsigned char)(w_be);
    ihdr[4] = (unsigned char)(h_be >> 24);
    ihdr[5] = (unsigned char)(h_be >> 16);
    ihdr[6] = (unsigned char)(h_be >> 8);
    ihdr[7] = (unsigned char)(h_be);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = write_alpha ? 6 : 2; /* color type: 6=RGBA, 2=RGB */
    ihdr[10] = 0; /* compression method */
    ihdr[11] = 0; /* filter method */
    ihdr[12] = 0; /* interlace method: none */
    if (write_chunk(f, "IHDR", ihdr, 13) != 0) {
        fclose(f);
        return -1;
    }

    /* tEXt chunks */
    for (int i = 0; i < n_kv; i++) {
        size_t klen = strlen(kv[i].key);
        size_t vlen = strlen(kv[i].value);
        size_t total = klen + 1 + vlen;
        unsigned char* buf = malloc(total);
        if (!buf) {
            fclose(f);
            return -1;
        }
        memcpy(buf, kv[i].key, klen);
        buf[klen] = 0;
        memcpy(buf + klen + 1, kv[i].value, vlen);
        int rc = write_chunk(f, "tEXt", buf, total);
        free(buf);
        if (rc != 0) {
            fclose(f);
            return -1;
        }
    }

    /* FAST PATH: Skip the CPU-heavy heuristic filter search entirely.
     * Use Filter 0 (None) and pack the pixels directly into the raw buffer. */
    size_t row_bytes = (size_t)width * bpp;
    size_t raw_len = (row_bytes + 1) * (size_t)height;
    unsigned char* raw = malloc(raw_len);
    if (!raw) {
        fclose(f);
        return -1;
    }

    for (int y = 0; y < height; y++) {
        unsigned char* dst_row = raw + (size_t)y * (row_bytes + 1);
        dst_row[0] = 0; /* PNG Filter Type 0: None */

        const uint8_t* src_row = pixels + (size_t)y * width * 4;

        if (write_alpha) {
            memcpy(dst_row + 1, src_row, row_bytes);
        } else {
            /* Strip the alpha channel directly into the compression buffer */
            unsigned char* p = dst_row + 1;
            for (int x = 0; x < width; x++) {
                p[x * 3 + 0] = src_row[x * 4 + 0];
                p[x * 3 + 1] = src_row[x * 4 + 1];
                p[x * 3 + 2] = src_row[x * 4 + 2];
            }
        }
    }

    uLongf comp_bound = compressBound((uLong)raw_len);
    unsigned char* comp = malloc(comp_bound);
    if (!comp) {
        free(raw);
        fclose(f);
        return -1;
    }

    uLongf comp_len = comp_bound;
    int zrc = compress2(comp, &comp_len, raw, (uLong)raw_len, 1);
    free(raw);

    if (zrc != Z_OK) {
        free(comp);
        fclose(f);
        return -1;
    }

    int rc = write_chunk(f, "IDAT", comp, comp_len);
    free(comp);
    if (rc != 0) {
        fclose(f);
        return -1;
    }

    if (write_chunk(f, "IEND", NULL, 0) != 0) {
        fclose(f);
        return -1;
    }

    if (fclose(f) != 0)
        return -1;
    return 0;
}

int png_read_text_chunks(const char* path, kv_pair_t* kv, int max_kv)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;

    unsigned char sig[8];
    if (fread(sig, 1, 8, f) != 8) {
        fclose(f);
        return -1;
    }
    static const unsigned char expected_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (memcmp(sig, expected_sig, 8) != 0) {
        fclose(f);
        return -1;
    }

    int n_found = 0;

    while (n_found < max_kv) {
        unsigned char len_be[4];
        if (fread(len_be, 1, 4, f) != 4)
            break;
        uint32_t len = ((uint32_t)len_be[0] << 24) | ((uint32_t)len_be[1] << 16) | ((uint32_t)len_be[2] << 8) | (uint32_t)len_be[3];

        char type[5] = { 0 };
        if (fread(type, 1, 4, f) != 4)
            break;

        if (strcmp(type, "IEND") == 0)
            break;

        if (strcmp(type, "tEXt") == 0 && len < 4096) {
            unsigned char* buf = malloc(len);
            if (!buf)
                break;
            if (fread(buf, 1, len, f) != len) {
                free(buf);
                break;
            }

            /* find the NUL separating key from value */
            size_t klen = 0;
            while (klen < len && buf[klen] != 0)
                klen++;
            if (klen < len && klen < sizeof(kv[n_found].key)) {
                memcpy(kv[n_found].key, buf, klen);
                kv[n_found].key[klen] = '\0';

                size_t vlen = len - klen - 1;
                if (vlen >= sizeof(kv[n_found].value))
                    vlen = sizeof(kv[n_found].value) - 1;
                memcpy(kv[n_found].value, buf + klen + 1, vlen);
                kv[n_found].value[vlen] = '\0';
                n_found++;
            }
            free(buf);
            fseek(f, 4, SEEK_CUR); /* CRC */
        } else {
            if (fseek(f, (long)len + 4, SEEK_CUR) != 0)
                break; /* data + CRC */
        }
    }

    fclose(f);
    return n_found;
}
