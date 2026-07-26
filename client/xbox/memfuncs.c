/* client/xbox/memfuncs.c
 *
 * Freestanding C library replacements for the Xbox loader (built with
 * -nostdlib -ffreestanding).  Same generic C as the PS2 port; the x86 CPU
 * has coherent, snooped caches so the DC-style cache helpers are no-ops
 * (the nForce NIC DMA is cache-coherent on this hardware).
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

/* ===== Common memfuncs interface used by shared code (commands.c) ===== */

void *fast_aligned_memcpy(void *dest, void *src, unsigned int numbytes) {
    return memcpy(dest, src, numbytes);
}

void *fast_memset_zero_64(void *dest, unsigned int count_64bit) {
    return memset(dest, 0, (size_t)count_64bit * 8);
}

/* x86 caches are coherent and snooped; nForce DMA is cache-coherent, so no
 * explicit cache maintenance is required for shared network buffers. */
void cache_block_purge(void *base, unsigned int count_32byte) {
    (void)base;
    (void)count_32byte;
}

void *to_cached(void *addr) {
    /* Flat, cached address space — no KSEG-style translation needed. */
    return addr;
}
