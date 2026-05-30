/* make-cd/tools/wadcrypto/md5.h */
/* Minimal MD5 (RFC 1321), public-domain implementation. Vendored for kos-tool's
 * Wii WAD packer (IMET + IMD5 hashes). Pure ISO C, no OS calls. */
#ifndef KOSTOOL_WADCRYPTO_MD5_H
#define KOSTOOL_WADCRYPTO_MD5_H

#include <stddef.h>
#include <stdint.h>

#define MD5_DIGEST_LEN 16

void md5(const void *data, size_t len, uint8_t out[MD5_DIGEST_LEN]);

#endif /* KOSTOOL_WADCRYPTO_MD5_H */
