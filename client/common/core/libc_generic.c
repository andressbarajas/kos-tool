/* client/common/core/libc_generic.c
 *
 * The generic freestanding memcpy/memmove/memset/memcmp/strlen shared 
 * by the most loaders (the Dreamcast keeps its own SH4 versions in 
 * client/dreamcast/memfuncs.c).
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
