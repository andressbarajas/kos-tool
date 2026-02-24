/* client/dreamcast/net/bswap.h */
#ifndef __BSWAP_H__
#define __BSWAP_H__

/* Based on dcload-ip: dcload-ip/target-src/dcload/bswap.h */

#if __GNUC__ <= 4

static inline unsigned short bswap16(unsigned short x) {
    __asm__ volatile("swap.b %[out], %[out]\n\t" : [out] "+r" (x) : : );
    return x;
}

static inline unsigned int bswap32(unsigned int x) {
    __asm__ volatile(
        "swap.b %[out], %[out]\n\t"
        "swap.w %[out], %[out]\n\t"
        "swap.b %[out], %[out]\n\t"
        : [out] "+r" (x) : : );
    return x;
}

#else

static inline unsigned short bswap16(unsigned short x) {
    return __builtin_bswap16(x);
}

static inline unsigned int bswap32(unsigned int x) {
    return __builtin_bswap32(x);
}

#endif

#endif /* __BSWAP_H__ */
