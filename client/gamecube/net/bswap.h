/* client/gamecube/net/bswap.h */
/*
 * Byte swap header for GameCube (PowerPC big-endian).
 * On a big-endian system, network byte order matches host byte order,
 * so byte swaps are identity operations.
 */

#ifndef __BSWAP_H__
#define __BSWAP_H__

static inline unsigned short bswap16(unsigned short x) { return x; }
static inline unsigned int bswap32(unsigned int x) { return x; }

#endif /* __BSWAP_H__ */
