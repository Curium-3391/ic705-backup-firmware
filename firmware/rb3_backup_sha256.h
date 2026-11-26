/*
 * rb3_backup_sha256.h - Freestanding zero-libc FIPS 180-4 SHA-256
 * Compiled for both Cortex-A9 ARM target and host verification.
 */
#ifndef RB3_BACKUP_SHA256_H
#define RB3_BACKUP_SHA256_H

#include <stdint.h>

#define RB3B_SHA256_BYTES 32u
#define RB3B_SHA256_HEX   65u

typedef struct {
    uint32_t state[8];
    uint32_t count_lo;
    uint32_t count_hi;
    uint8_t  buf[64];
} rb3b_sha256_ctx_t;

void rb3b_sha256_init(rb3b_sha256_ctx_t *ctx);
void rb3b_sha256_update(rb3b_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void rb3b_sha256_final(rb3b_sha256_ctx_t *ctx, uint8_t out_digest[RB3B_SHA256_BYTES]);
void rb3b_sha256_hex(const uint8_t digest[RB3B_SHA256_BYTES], char out_hex[RB3B_SHA256_HEX]);

#endif /* RB3_BACKUP_SHA256_H */
