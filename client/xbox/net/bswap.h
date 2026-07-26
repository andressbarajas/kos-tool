/* client/xbox/net/bswap.h — byte swaps for Xbox (x86, little-endian). */
#ifndef __BSWAP_H__
#define __BSWAP_H__

static inline unsigned short bswap16(unsigned short x) {
    return __builtin_bswap16(x);
}

static inline unsigned int bswap32(unsigned int x) {
    return __builtin_bswap32(x);
}

#endif /* __BSWAP_H__ */
