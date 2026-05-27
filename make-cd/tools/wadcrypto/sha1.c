/* make-cd/tools/wadcrypto/sha1.c */
/* Minimal SHA-1, public-domain implementation. */

#include "sha1.h"
#include <string.h>

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(uint32_t state[5], const uint8_t blk[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)blk[i*4] << 24) | ((uint32_t)blk[i*4+1] << 16) |
           ((uint32_t)blk[i*4+2] << 8) | (uint32_t)blk[i*4+3];
  for (int i = 16; i < 80; i++)
    w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5a827999; }
    else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8f1bbcdc; }
    else             { f = b ^ c ^ d;                    k = 0xca62c1d6; }
    uint32_t tmp = rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol(b, 30); b = a; a = tmp;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_init(sha1_ctx *c) {
  c->state[0] = 0x67452301; c->state[1] = 0xefcdab89; c->state[2] = 0x98badcfe;
  c->state[3] = 0x10325476; c->state[4] = 0xc3d2e1f0;
  c->count = 0; c->buflen = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  c->count += len;
  if (c->buflen) {
    size_t need = 64 - c->buflen;
    size_t take = len < need ? len : need;
    memcpy(c->buf + c->buflen, p, take);
    c->buflen += take; p += take; len -= take;
    if (c->buflen == 64) { sha1_block(c->state, c->buf); c->buflen = 0; }
  }
  while (len >= 64) { sha1_block(c->state, p); p += 64; len -= 64; }
  if (len) { memcpy(c->buf, p, len); c->buflen = len; }
}

void sha1_final(sha1_ctx *c, uint8_t out[20]) {
  uint64_t bits = c->count * 8;
  uint8_t pad = 0x80;
  sha1_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->buflen != 56) sha1_update(c, &zero, 1);
  uint8_t lenb[8];
  for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8*i));
  sha1_update(c, lenb, 8);
  for (int i = 0; i < 5; i++) {
    out[i*4]   = (uint8_t)(c->state[i] >> 24);
    out[i*4+1] = (uint8_t)(c->state[i] >> 16);
    out[i*4+2] = (uint8_t)(c->state[i] >> 8);
    out[i*4+3] = (uint8_t)(c->state[i]);
  }
}

void sha1(const void *data, size_t len, uint8_t out[20]) {
  sha1_ctx c; sha1_init(&c); sha1_update(&c, data, len); sha1_final(&c, out);
}
