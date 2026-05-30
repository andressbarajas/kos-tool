/* make-cd/tools/wadcrypto/aes.h */
/*
 * Minimal AES-128 (ECB block ops + CBC mode), public-domain implementation.
 *
 * Vendored for kos-tool's Wii WAD packer (host side only). Pure ISO C, no OS
 * calls, no external libraries, no RNG — portable to every host kos-tool
 * targets (Linux/macOS/Windows-MinGW). Not a console SDK; clean-room-safe.
 */
#ifndef KOSTOOL_WADCRYPTO_AES_H
#define KOSTOOL_WADCRYPTO_AES_H

#include <stddef.h>
#include <stdint.h>

#define AES128_KEYLEN   16
#define AES128_BLOCKLEN 16

/* Single 16-byte block, in place, using a pre-expanded 176-byte key schedule. */
void aes128_key_expand(const uint8_t key[AES128_KEYLEN], uint8_t rk[176]);
void aes128_encrypt_block(const uint8_t rk[176], uint8_t block[AES128_BLOCKLEN]);
void aes128_decrypt_block(const uint8_t rk[176], uint8_t block[AES128_BLOCKLEN]);

/* CBC over a buffer whose length must be a multiple of 16. iv is not modified;
 * the operation is in place on buf. */
void aes128_cbc_encrypt(const uint8_t key[AES128_KEYLEN], const uint8_t iv[AES128_BLOCKLEN], uint8_t *buf,
                        size_t len);
void aes128_cbc_decrypt(const uint8_t key[AES128_KEYLEN], const uint8_t iv[AES128_BLOCKLEN], uint8_t *buf,
                        size_t len);

#endif /* KOSTOOL_WADCRYPTO_AES_H */
