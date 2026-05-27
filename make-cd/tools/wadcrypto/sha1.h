/* make-cd/tools/wadcrypto/sha1.h */
/* Minimal SHA-1, public-domain implementation. Vendored for kos-tool's Wii
 * WAD packer (host side). Pure ISO C, no OS calls. Clean-room-safe. */
#ifndef KOSTOOL_WADCRYPTO_SHA1_H
#define KOSTOOL_WADCRYPTO_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_LEN 20

typedef struct {
  uint32_t state[5];
  uint64_t count;     /* total bytes processed */
  uint8_t  buf[64];
  size_t   buflen;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
void sha1_final(sha1_ctx *c, uint8_t out[SHA1_DIGEST_LEN]);

/* One-shot convenience. */
void sha1(const void *data, size_t len, uint8_t out[SHA1_DIGEST_LEN]);

#endif /* KOSTOOL_WADCRYPTO_SHA1_H */
