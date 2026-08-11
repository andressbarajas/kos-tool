/* client/psp/memfuncs.c — freestanding C runtime helpers for the PSP loader. */

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

/* ===== Common memfuncs interface used by shared code (commands.c) ===== */

void *fast_aligned_memcpy(void *dest, void *src, unsigned int numbytes) {
    return memcpy(dest, src, numbytes);
}

void *fast_memset_zero_64(void *dest, unsigned int count_64bit) {
    return memset(dest, 0, (size_t)count_64bit * 8);
}

extern void cache_flush_range(const void *addr, unsigned int size);

void cache_block_purge(void *base, unsigned int count_32byte) {
    cache_flush_range(base, count_32byte * 32);
}

void *to_cached(void *addr) {
    /* PSP allocator identities are canonical KU0 pointers.  This helper is
     * intentionally KU0/KU1-only: it strips the uncached-view bit, but does
     * not turn a diagnostic K0/K1 address into an allocator pointer. */
    return (void *)((unsigned int)addr & ~0x40000000u);
}
