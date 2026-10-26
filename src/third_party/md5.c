/* Minimal MD5 implementation, written from the public RFC 1321 algorithm
 * description. Public domain / CC0. */
#include "md5.h"
#include <string.h>
#include <stdio.h>

static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
    0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
    0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
    0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
    0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
    0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static const int S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static uint32_t rotl32(uint32_t x, int c) {
    return (x << c) | (x >> (32 - c));
}

static void md5_process_block(md5_ctx_t *ctx, const unsigned char block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = (uint32_t)block[i*4]       |
               (uint32_t)block[i*4+1]<<8  |
               (uint32_t)block[i*4+2]<<16 |
               (uint32_t)block[i*4+3]<<24;
    }

    uint32_t A = ctx->state[0], B = ctx->state[1],
             C = ctx->state[2], D = ctx->state[3];

    for (int i = 0; i < 64; i++) {
        uint32_t F;
        int g;
        if (i < 16) {
            F = (B & C) | (~B & D);
            g = i;
        } else if (i < 32) {
            F = (D & B) | (~D & C);
            g = (5*i + 1) % 16;
        } else if (i < 48) {
            F = B ^ C ^ D;
            g = (3*i + 5) % 16;
        } else {
            F = C ^ (B | ~D);
            g = (7*i) % 16;
        }
        F = F + A + K[i] + M[g];
        A = D;
        D = C;
        C = B;
        B = B + rotl32(F, S[i]);
    }

    ctx->state[0] += A;
    ctx->state[1] += B;
    ctx->state[2] += C;
    ctx->state[3] += D;
}

void md5_init(md5_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->bitcount = 0;
}

void md5_update(md5_ctx_t *ctx, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    size_t buf_used = (size_t)((ctx->bitcount / 8) % 64);
    ctx->bitcount += (uint64_t)len * 8;

    size_t i = 0;
    if (buf_used) {
        size_t fill = 64 - buf_used;
        size_t take = len < fill ? len : fill;
        memcpy(ctx->buffer + buf_used, p, take);
        i += take;
        buf_used += take;
        if (buf_used == 64) {
            md5_process_block(ctx, ctx->buffer);
            buf_used = 0;
        }
    }

    for (; i + 64 <= len; i += 64) {
        md5_process_block(ctx, p + i);
    }

    if (i < len) {
        memcpy(ctx->buffer, p + i, len - i);
    }
}

void md5_final(md5_ctx_t *ctx, unsigned char digest[16]) {
    unsigned char pad[72];
    size_t buf_used = (size_t)((ctx->bitcount / 8) % 64);
    uint64_t bitcount = ctx->bitcount;

    size_t padlen = (buf_used < 56) ? (56 - buf_used) : (120 - buf_used);
    pad[0] = 0x80;
    memset(pad + 1, 0, padlen - 1);
    md5_update(ctx, pad, padlen);

    /* undo the bitcount bump from md5_update above; write true length */
    ctx->bitcount = bitcount + padlen * 8;

    unsigned char lenbytes[8];
    for (int i = 0; i < 8; i++) {
        lenbytes[i] = (unsigned char)((bitcount >> (8*i)) & 0xff);
    }
    md5_update(ctx, lenbytes, 8);

    for (int i = 0; i < 4; i++) {
        digest[i*4+0] = (unsigned char)(ctx->state[i] & 0xff);
        digest[i*4+1] = (unsigned char)((ctx->state[i] >> 8) & 0xff);
        digest[i*4+2] = (unsigned char)((ctx->state[i] >> 16) & 0xff);
        digest[i*4+3] = (unsigned char)((ctx->state[i] >> 24) & 0xff);
    }
}

void md5_hex_string(const char *str, char out[33]) {
    md5_ctx_t ctx;
    unsigned char digest[16];
    md5_init(&ctx);
    md5_update(&ctx, str, strlen(str));
    md5_final(&ctx, digest);
    for (int i = 0; i < 16; i++) {
        snprintf(out + i*2, 3, "%02x", digest[i]);
    }
}
