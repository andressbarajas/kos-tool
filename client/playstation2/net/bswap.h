/* client/playstation2/net/bswap.h */
/*
 * Byte-swap functions for PS2 (MIPS R5900, little-endian).
 * Real byte swaps needed for network byte order conversion.
 */

#ifndef __BSWAP_H__
#define __BSWAP_H__

static inline unsigned short bswap16(unsigned short x) {
	return __builtin_bswap16(x);
}

static inline unsigned int bswap32(unsigned int x) {
	return __builtin_bswap32(x);
}

#endif /* __BSWAP_H__ */
