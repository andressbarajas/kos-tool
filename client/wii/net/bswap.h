/* client/wii/net/bswap.h - Wii PPC is big-endian. */
#ifndef KOSLOAD_WII_BSWAP_H
#define KOSLOAD_WII_BSWAP_H

static inline unsigned short bswap16(unsigned short x) { return x; }
static inline unsigned int bswap32(unsigned int x) { return x; }

#endif /* KOSLOAD_WII_BSWAP_H */
