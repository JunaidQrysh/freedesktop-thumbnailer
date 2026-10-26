/* Minimal MD5 implementation, written from the public RFC 1321 algorithm
 * description. This code is placed in the public domain / released under
 * CC0 - use it for anything. No relation to any particular vendor's
 * reference code. */
#ifndef MD5_H
#define MD5_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[4];
    uint64_t bitcount;
    unsigned char buffer[64];
} md5_ctx_t;

void md5_init(md5_ctx_t *ctx);
void md5_update(md5_ctx_t *ctx, const void *data, size_t len);
void md5_final(md5_ctx_t *ctx, unsigned char digest[16]);

/* Convenience: hash a NUL-terminated string and return 32 lowercase hex
 * chars + NUL in out (out must have room for 33 bytes). */
void md5_hex_string(const char *str, char out[33]);

#endif
