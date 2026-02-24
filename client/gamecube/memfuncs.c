/* client/gamecube/memfuncs.c */
/*
 * Freestanding C library replacements for GameCube.
 *
 * Provides memcpy, memset, memcmp, memmove, strlen, and __eabi
 * since we build with -nostdlib -ffreestanding.
 */

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

void *memset(void *dest, int c, size_t n)
{
    unsigned char *d = dest;
    while (n--)
        *d++ = (unsigned char)c;
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

/*
 * __eabi - EABI initialization function.
 *
 * GCC inserts a call to __eabi() at the start of main() when -meabi
 * is used. In a full runtime it initializes .ctors/.dtors and the
 * small data areas (r2/r13). For our bare-metal loader, crt0.S
 * handles all initialization, so this is a no-op stub.
 */
void __eabi(void)
{
}

/*
 * Common memfuncs interface — used by shared code (commands.c).
 * GC uses standard library calls; platform-specific optimizations
 * can be added here in the future.
 */

void *fast_aligned_memcpy(void *dest, void *src, unsigned int numbytes)
{
    return memcpy(dest, src, numbytes);
}

void *fast_memset_zero_64(void *dest, unsigned int count_64bit)
{
    return memset(dest, 0, count_64bit * 8);
}

extern void cache_flush_dc(const void *addr, unsigned int size);

void cache_block_purge(void *base, unsigned int count_32byte)
{
    cache_flush_dc(base, count_32byte * 32);
}

void *to_cached(void *addr)
{
    return addr;
}
