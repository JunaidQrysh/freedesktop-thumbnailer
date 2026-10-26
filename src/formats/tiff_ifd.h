#ifndef TIFF_IFD_H
#define TIFF_IFD_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const uint8_t *data;
    size_t len;
    int little_endian; /* 1 if "II" byte order, 0 if "MM" */
} tiff_reader_t;

typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t value_raw; /* raw 4-byte field: value itself (if it fits) or an offset */
} tiff_entry_t;

/* data must point at the start of a TIFF header ("II*\0" or "MM\0*...").
 * Returns 1 and fills r + *ifd0_offset on success, 0 on malformed header. */
int tiff_reader_init(tiff_reader_t *r, const uint8_t *data, size_t len, uint32_t *ifd0_offset);

/* Reads the IFD at ifd_offset into entries[] (up to max_entries), returns
 * the actual entry count (may exceed max_entries; only max_entries are
 * written), or -1 on malformed data. *next_ifd_offset gets the offset of
 * the next IFD in the chain (0 if none). */
int tiff_read_ifd(const tiff_reader_t *r, uint32_t ifd_offset,
                   tiff_entry_t *entries, int max_entries,
                   uint32_t *next_ifd_offset);

uint32_t tiff_entry_u32(const tiff_reader_t *r, const tiff_entry_t *e);
uint16_t tiff_entry_u16(const tiff_reader_t *r, const tiff_entry_t *e);

/* Convenience: find the value of `tag` within the IFD entries array
 * (already read via tiff_read_ifd). Returns 1 and sets *out if found. */
int tiff_find_tag(const tiff_entry_t *entries, int n, uint16_t tag, tiff_entry_t *out);

/* Scans IFD0, any SubIFDs it references (tag 0x014A), and the whole
 * top-level IFD chain (IFD0 -> IFD1 -> ...), looking for
 * JPEGInterchangeFormat (0x0201) / JPEGInterchangeFormatLength (0x0202)
 * pairs, and returns the LARGEST embedded JPEG found (RAW files often
 * carry multiple previews of different sizes). Returns 1 and sets
 * jpg_offset / jpg_len (offsets are absolute within `data`) on success,
 * 0 if none found. */
int tiff_find_largest_jpeg(const uint8_t *data, size_t len,
                            uint32_t *jpg_offset, uint32_t *jpg_len);

/* Reads the EXIF Orientation tag (0x0112) from IFD0. Returns 1-8 (the
 * standard EXIF orientation values) or 1 (identity) if not present/on
 * error - orientation 1 means "do nothing", so this is a safe default. */
int tiff_read_orientation(const uint8_t *data, size_t len);

/* Scans IFD0, its SubIFDs, and the top-level IFD chain (same traversal
 * as tiff_find_largest_jpeg) for ImageWidth (0x0100) / ImageLength
 * (0x0101) tag pairs, and returns the LARGEST one found by pixel area.
 * RAW files often describe a small thumbnail in IFD0 and the real
 * sensor resolution in a SubIFD (or vice versa) - taking the largest
 * gives the true original resolution rather than whichever IFD's
 * embedded preview we happened to extract for the actual thumbnail
 * pixels. Returns 1 and sets *width / *height on success, 0 if no
 * ImageWidth/ImageLength pair was found anywhere. */
int tiff_find_largest_dimensions(const uint8_t *data, size_t len, int *width, int *height);

#endif
