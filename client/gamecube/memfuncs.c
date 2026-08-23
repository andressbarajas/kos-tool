/* client/gamecube/memfuncs.c */
/*
 * GameCube-specific runtime helpers.
 *
 * The generic freestanding memcpy/memmove/memset/memcmp/strlen live in
 * client/common/core/libc_generic.c, shared with the other ports.
 */

#include <stddef.h>
#include <string.h>

/*
 * __eabi - EABI initialization function.
 *
 * GCC inserts a call to __eabi() at the start of main() when -meabi
 * is used. In a full runtime it initializes .ctors/.dtors and the
 * small data areas (r2/r13). For our bare-metal loader, crt0.S
 * handles all initialization, so this is a no-op stub.
 */
void __eabi(void) {
}

/*
 * Common memfuncs interface — used by shared code (commands.c).
 * GC uses standard library calls; platform-specific optimizations
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
    cache_flush_dc(base, count_32byte * 32);
}

void *to_cached(void *addr) {
    return addr;
}
