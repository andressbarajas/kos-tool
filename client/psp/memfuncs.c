/* client/psp/memfuncs.c — PSP-specific runtime helpers.
 *
 * The generic freestanding memcpy/memmove/memset/memcmp/strlen live in
 * client/common/core/libc_generic.c, shared with the other ports.
 */

#include <stddef.h>
#include <string.h>

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
