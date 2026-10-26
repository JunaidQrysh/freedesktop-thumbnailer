#include "isobmff.h"

static uint32_t read_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t read_u64be(const uint8_t *p) {
    uint64_t hi = read_u32be(p);
    uint64_t lo = read_u32be(p + 4);
    return (hi << 32) | lo;
}

int isobmff_foreach_box(const uint8_t *data, size_t len, isobmff_box_cb cb, void *ctx) {
    size_t pos = 0;
    int n = 0;
    while (pos + 8 <= len) {
        uint32_t size32 = read_u32be(data + pos);
        uint32_t type = read_u32be(data + pos + 4);
        uint64_t size = size32;
        size_t header_len = 8;

        if (size32 == 1) {
            if (pos + 16 > len) break;
            size = read_u64be(data + pos + 8);
            header_len = 16;
        } else if (size32 == 0) {
            size = len - pos; /* extends to end of buffer */
        }

        if (size < header_len || pos + size > len) break;

        isobmff_box_t box;
        box.type = type;
        box.box_start = data + pos;
        box.body = data + pos + header_len;
        box.body_len = (size_t)size - header_len;

        n++;
        if (cb && !cb(&box, ctx)) break;

        pos += (size_t)size;
    }
    return n;
}

typedef struct {
    uint32_t want_type;
    isobmff_box_t *out;
    int found;
} find_ctx_t;

static int find_cb(const isobmff_box_t *box, void *vctx) {
    find_ctx_t *ctx = (find_ctx_t *)vctx;
    if (box->type == ctx->want_type) {
        *ctx->out = *box;
        ctx->found = 1;
        return 0; /* stop */
    }
    return 1;
}

int isobmff_find_box(const uint8_t *data, size_t len, uint32_t fourcc, isobmff_box_t *out) {
    find_ctx_t ctx = { fourcc, out, 0 };
    isobmff_foreach_box(data, len, find_cb, &ctx);
    return ctx.found;
}
