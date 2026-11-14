/*
 * OmniOS — os/lib/sha256.h: SHA-256 (sha256.c)
 */
#ifndef OMNI_OS_LIB_SHA256_H
#define OMNI_OS_LIB_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
};

void sha_init(struct sha256 *s);
void sha_update(struct sha256 *s, const unsigned char *p, size_t len);
void sha_final(struct sha256 *s, char hex[65]);          /* lowercase hex */
void sha_final_bin(struct sha256 *s, unsigned char out[32]);
int  sha256_file(const char *path, char hex[65]);        /* 0 ok, -1 error */
void sha256_hex(const void *data, size_t n, char hex[65]);

#endif /* OMNI_OS_LIB_SHA256_H */
