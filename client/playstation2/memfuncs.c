/* client/playstation2/memfuncs.c */
/*
 * Freestanding C library replacements for PlayStation 2.
 *
 * Provides memcpy, memset, memcmp, memmove, strlen since we build
 * with -nostdlib -ffreestanding.
 */

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    while(n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = dest;
    const unsigned char *s = src;
    if(d < s) {
        while(n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while(n--)
            *--d = *--s;
    }
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    unsigned char *d = dest;
    while(n--)
        *d++ = (unsigned char)c;
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while(n--) {
        if(*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while(*p)
        p++;
    return p - s;
}

/*
 * Common memfuncs interface -- used by shared code (commands.c).
 * PS2 uses standard library calls; platform-specific optimizations
 * can be added here in the future.
 */

void *fast_aligned_memcpy(void *dest, void *src, unsigned int numbytes) {
    return memcpy(dest, src, numbytes);
}

void *fast_memset_zero_64(void *dest, unsigned int count_64bit) {
    return memset(dest, 0, count_64bit * 8);
}

extern void cache_flush_dc(const void *addr, unsigned int size);

void cache_block_purge(void *base, unsigned int count_32byte) {
    /* Interface is defined in 32-byte blocks for DC compatibility.
     * On PS2, cache_flush_dc aligns to 64-byte cache lines internally,
     * so flushing a 32-byte-granularity range still works correctly. */
    cache_flush_dc(base, count_32byte * 32);
}

void *to_cached(void *addr) {
    /* On PS2 EE, KSEG0 addresses (0x80000000+) are already cached.
     * User-space addresses in KUSEG (0x00000000-0x7FFFFFFF) are also
     * cached by default. Return address unchanged. */
    return addr;
}
