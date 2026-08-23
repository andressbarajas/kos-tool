/* client/xbox/memfuncs.c
 *
 * Xbox-specific runtime helpers.  The x86 CPU has coherent, snooped caches
 * so the DC-style cache helpers are no-ops (the nForce NIC DMA is
 * cache-coherent on this hardware).
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
