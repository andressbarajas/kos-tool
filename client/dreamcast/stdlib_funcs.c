/* client/dreamcast/stdlib_funcs.c */
/*
 * Freestanding C library replacements for Dreamcast.
 *
 * Provides memcpy, memset, memcmp, memmove, strlen since we
 * build with -nostdlib -ffreestanding. In a separate file from
 * memfuncs.c so the SH4-optimized packet transfer functions
 * don't get pulled into serial builds that only need these.
 */

#include <stddef.h>

/* memcpy is provided by memcpy.S (fast SH4 ASM implementation) */

void *memset(void *dest, int c, size_t n)
{
    unsigned char *d = dest;
    while (n--)
        *d++ = (unsigned char)c;
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
