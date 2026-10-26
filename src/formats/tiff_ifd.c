#include "tiff_ifd.h"
#include <string.h>

static uint16_t read_u16(const tiff_reader_t *r, size_t off) {
    if (off + 2 > r->len) return 0;
    const uint8_t *p = r->data + off;
    return r->little_endian ? (uint16_t)(p[0] | (p[1] << 8))
                             : (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_u32(const tiff_reader_t *r, size_t off) {
    if (off + 4 > r->len) return 0;
    const uint8_t *p = r->data + off;
    return r->little_endian
        ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
        : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

int tiff_reader_init(tiff_reader_t *r, const uint8_t *data, size_t len, uint32_t *ifd0_offset) {
    if (len < 8) return 0;
    int little;
    if (data[0] == 'I' && data[1] == 'I') little = 1;
    else if (data[0] == 'M' && data[1] == 'M') little = 0;
    else return 0;

    r->data = data;
    r->len = len;
    r->little_endian = little;

    uint16_t magic = read_u16(r, 2);
    if (magic != 42) return 0;

    *ifd0_offset = read_u32(r, 4);
    return 1;
}

int tiff_read_ifd(const tiff_reader_t *r, uint32_t ifd_offset,
                   tiff_entry_t *entries, int max_entries,
                   uint32_t *next_ifd_offset) {
    if (ifd_offset == 0 || (size_t)ifd_offset + 2 > r->len) return -1;
    uint16_t count = read_u16(r, ifd_offset);
    size_t entries_start = ifd_offset + 2;
    if (entries_start + (size_t)count * 12 + 4 > r->len) {
        /* tolerate truncated next-IFD pointer, but bail if entries themselves don't fit */
        if (entries_start + (size_t)count * 12 > r->len) return -1;
    }

    int n_write = count < max_entries ? count : max_entries;
    for (int i = 0; i < n_write; i++) {
        size_t eoff = entries_start + (size_t)i * 12;
        entries[i].tag = read_u16(r, eoff);
        entries[i].type = read_u16(r, eoff + 2);
        entries[i].count = read_u32(r, eoff + 4);
        entries[i].value_raw = read_u32(r, eoff + 8);
    }

    size_t next_off_pos = entries_start + (size_t)count * 12;
    *next_ifd_offset = (next_off_pos + 4 <= r->len) ? read_u32(r, next_off_pos) : 0;

    return count;
}

uint32_t tiff_entry_u32(const tiff_reader_t *r, const tiff_entry_t *e) {
    (void)r;
    return e->value_raw;
}

uint16_t tiff_entry_u16(const tiff_reader_t *r, const tiff_entry_t *e) {
    return r->little_endian ? (uint16_t)(e->value_raw & 0xFFFF)
                             : (uint16_t)((e->value_raw >> 16) & 0xFFFF);
}

int tiff_find_tag(const tiff_entry_t *entries, int n, uint16_t tag, tiff_entry_t *out) {
    for (int i = 0; i < n; i++) {
        if (entries[i].tag == tag) {
            *out = entries[i];
            return 1;
        }
    }
    return 0;
}

#define TAG_SUBIFDS 0x014A
#define TAG_JPEG_OFFSET 0x0201
#define TAG_JPEG_LENGTH 0x0202
#define MAX_IFD_ENTRIES 64

static void scan_ifd_for_jpeg(const tiff_reader_t *r, uint32_t ifd_offset, int depth,
                               uint32_t *best_off, uint32_t *best_len) {
    if (depth > 4 || ifd_offset == 0) return; /* guard against cycles */

    tiff_entry_t entries[MAX_IFD_ENTRIES];
    uint32_t next_ifd;
    int n = tiff_read_ifd(r, ifd_offset, entries, MAX_IFD_ENTRIES, &next_ifd);
    if (n < 0) return;
    int n_avail = n < MAX_IFD_ENTRIES ? n : MAX_IFD_ENTRIES;

    tiff_entry_t off_e, len_e;
    if (tiff_find_tag(entries, n_avail, TAG_JPEG_OFFSET, &off_e) &&
        tiff_find_tag(entries, n_avail, TAG_JPEG_LENGTH, &len_e)) {
        uint32_t off = tiff_entry_u32(r, &off_e);
        uint32_t jlen = tiff_entry_u32(r, &len_e);
        if (jlen > 0 && (size_t)off + jlen <= r->len && jlen > *best_len) {
            *best_off = off;
            *best_len = jlen;
        }
    }

    /* descend into SubIFDs, if any */
    tiff_entry_t sub_e;
    if (tiff_find_tag(entries, n_avail, TAG_SUBIFDS, &sub_e)) {
        if (sub_e.count == 1) {
            scan_ifd_for_jpeg(r, tiff_entry_u32(r, &sub_e), depth + 1, best_off, best_len);
        } else if (sub_e.type == 4 || sub_e.type == 3) {
            /* array of offsets stored at value_raw (as an offset into the file) */
            uint32_t arr_off = sub_e.value_raw;
            for (uint32_t i = 0; i < sub_e.count; i++) {
                uint32_t sub_ifd_off = read_u32(r, arr_off + i * 4);
                scan_ifd_for_jpeg(r, sub_ifd_off, depth + 1, best_off, best_len);
            }
        }
    }

    /* continue the top-level IFD chain */
    if (next_ifd != 0) {
        scan_ifd_for_jpeg(r, next_ifd, depth + 1, best_off, best_len);
    }
}

int tiff_find_largest_jpeg(const uint8_t *data, size_t len,
                            uint32_t *jpg_offset, uint32_t *jpg_len) {
    tiff_reader_t r;
    uint32_t ifd0;
    if (!tiff_reader_init(&r, data, len, &ifd0)) return 0;

    uint32_t best_off = 0, best_len = 0;
    scan_ifd_for_jpeg(&r, ifd0, 0, &best_off, &best_len);

    if (best_len == 0) return 0;
    *jpg_offset = best_off;
    *jpg_len = best_len;
    return 1;
}

#define TAG_IMAGE_WIDTH 0x0100
#define TAG_IMAGE_LENGTH 0x0101

static uint32_t tiff_entry_generic_u32(const tiff_reader_t *r, const tiff_entry_t *e) {
    /* ImageWidth/ImageLength are SHORT (type 3) or LONG (type 4) per spec */
    return (e->type == 3) ? (uint32_t)tiff_entry_u16(r, e) : tiff_entry_u32(r, e);
}

static void scan_ifd_for_dimensions(const tiff_reader_t *r, uint32_t ifd_offset, int depth,
                                     uint32_t *best_w, uint32_t *best_h) {
    if (depth > 4 || ifd_offset == 0) return; /* guard against cycles */

    tiff_entry_t entries[MAX_IFD_ENTRIES];
    uint32_t next_ifd;
    int n = tiff_read_ifd(r, ifd_offset, entries, MAX_IFD_ENTRIES, &next_ifd);
    if (n < 0) return;
    int n_avail = n < MAX_IFD_ENTRIES ? n : MAX_IFD_ENTRIES;

    tiff_entry_t w_e, h_e;
    if (tiff_find_tag(entries, n_avail, TAG_IMAGE_WIDTH, &w_e) &&
        tiff_find_tag(entries, n_avail, TAG_IMAGE_LENGTH, &h_e)) {
        uint32_t w = tiff_entry_generic_u32(r, &w_e);
        uint32_t h = tiff_entry_generic_u32(r, &h_e);
        if (w > 0 && h > 0 && (uint64_t)w * h > (uint64_t)(*best_w) * (*best_h)) {
            *best_w = w;
            *best_h = h;
        }
    }

    tiff_entry_t sub_e;
    if (tiff_find_tag(entries, n_avail, TAG_SUBIFDS, &sub_e)) {
        if (sub_e.count == 1) {
            scan_ifd_for_dimensions(r, tiff_entry_u32(r, &sub_e), depth + 1, best_w, best_h);
        } else if (sub_e.type == 4 || sub_e.type == 3) {
            uint32_t arr_off = sub_e.value_raw;
            for (uint32_t i = 0; i < sub_e.count; i++) {
                uint32_t sub_ifd_off = read_u32(r, arr_off + i * 4);
                scan_ifd_for_dimensions(r, sub_ifd_off, depth + 1, best_w, best_h);
            }
        }
    }

    if (next_ifd != 0) {
        scan_ifd_for_dimensions(r, next_ifd, depth + 1, best_w, best_h);
    }
}

int tiff_find_largest_dimensions(const uint8_t *data, size_t len, int *width, int *height) {
    tiff_reader_t r;
    uint32_t ifd0;
    if (!tiff_reader_init(&r, data, len, &ifd0)) return 0;

    uint32_t best_w = 0, best_h = 0;
    scan_ifd_for_dimensions(&r, ifd0, 0, &best_w, &best_h);

    if (best_w == 0 || best_h == 0) return 0;
    *width = (int)best_w;
    *height = (int)best_h;
    return 1;
}

#define TAG_ORIENTATION 0x0112

int tiff_read_orientation(const uint8_t *data, size_t len) {
    tiff_reader_t r;
    uint32_t ifd0;
    if (!tiff_reader_init(&r, data, len, &ifd0)) return 1;

    tiff_entry_t entries[MAX_IFD_ENTRIES];
    uint32_t next_ifd;
    int n = tiff_read_ifd(&r, ifd0, entries, MAX_IFD_ENTRIES, &next_ifd);
    if (n < 0) return 1;
    int n_avail = n < MAX_IFD_ENTRIES ? n : MAX_IFD_ENTRIES;

    tiff_entry_t orient_e;
    if (!tiff_find_tag(entries, n_avail, TAG_ORIENTATION, &orient_e)) return 1;

    uint16_t v = tiff_entry_u16(&r, &orient_e);
    if (v < 1 || v > 8) return 1;
    return v;
}
