/* client/playstation2/memfuncs.c */
/*
 * PlayStation 2-specific runtime helpers.
 *
 * The generic freestanding memcpy/memmove/memset/memcmp/strlen live in
 * client/common/core/libc_generic.c, shared with the other ports.
 */

#include <stddef.h>
#include <string.h>

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
