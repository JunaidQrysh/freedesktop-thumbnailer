#ifndef ISOBMFF_H
#define ISOBMFF_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t type;      /* fourcc as big-endian packed uint32, compare with ISOBMFF_FOURCC() */
    const uint8_t *body; /* points just past the box header, i.e. the box's payload */
    size_t body_len;
    const uint8_t *box_start; /* start of the box including header - for computing absolute offsets */
} isobmff_box_t;

#define ISOBMFF_FOURCC(a,b,c,d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Finds the first top-level box of the given fourcc within [data, data+len).
 * Non-recursive - only looks at boxes directly at this level. Returns 1
 * and fills *out on success, 0 if not found / malformed. */
int isobmff_find_box(const uint8_t *data, size_t len, uint32_t fourcc, isobmff_box_t *out);

/* Iterates ALL boxes at this level, calling back for each. Callback
 * returns 0 to stop iteration early, nonzero to continue. Returns the
 * number of boxes visited. */
typedef int (*isobmff_box_cb)(const isobmff_box_t *box, void *ctx);
int isobmff_foreach_box(const uint8_t *data, size_t len, isobmff_box_cb cb, void *ctx);

#endif
