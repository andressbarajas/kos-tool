/* client/psp/cache.h — Allegrex (MIPS) cache maintenance for the PSP loader.
 *
 * The Allegrex has separate I/D caches with 64-byte lines.  For coherency with
 * uploaded program code and with USB DMA buffers we write back + invalidate the
 * affected D-cache range and invalidate the I-cache.  Shared USB buffers are
 * additionally accessed through the normalized KU1 alias
 * ((addr & 0x1fffffff) | 0x40000000) in the USB HAL, so this is
 * belt-and-suspenders. Canonical allocator pointers stay in KU0.
 *
 * Opcodes verified against retail 6.61, NOT inferred:
 *
 *   0x1A  hit D-cache writeback, line stays valid   (sysmem.prx text+0x864)
 *   0x1B  hit D-cache writeback + invalidate        (sysmem.prx text+0x960)
 *   0x08  hit I-cache invalidate                    (sysmem.prx text+0xF60)
 *   0x14  INDEXED whole-cache op, twice per index   (IPL 0x04003414) -- indexed,
 *         so it must never be used as a by-address operation.
 *
 * The pair below is the sequence Sony itself uses to publish CPU-written code,
 * observed directly in interruptman.prx text+0x3268:
 *
 *     sw    <insn>, N(at)
 *     cache 0x1b, N(at)
 *     cache 0x08, N(at)
 *     ...
 *     sync
 *
 * Line size is 64 bytes; Sony's range loops step by 64 and end with one `sync`.
 */

#ifndef KOSLOAD_PSP_CACHE_H
#define KOSLOAD_PSP_CACHE_H

#include <stddef.h>

#define PSP_CACHE_LINE 64

static inline void cache_flush_range(const void *addr, unsigned int size) {
    unsigned int a = (unsigned int)addr & ~(PSP_CACHE_LINE - 1);
    unsigned int end = ((unsigned int)addr + size + PSP_CACHE_LINE - 1) & ~(PSP_CACHE_LINE - 1);
    for(; a < end; a += PSP_CACHE_LINE) {
        /* The `cache` opcode isn't in the mips2 baseline this toolchain
         * defaults to; scope just these two instructions to mips32 (the
         * Allegrex is MIPS32-class) without changing the float/ABI defaults. */
        __asm__ volatile(".set push\n"
                         ".set mips32\n"
                         "cache 0x1b, 0(%0)\n"  /* D: hit writeback+invalidate */
                         "cache 0x08, 0(%0)\n"  /* I: hit invalidate           */
                         ".set pop\n"
                         :
                         : "r"(a)
                         : "memory");
    }
    __asm__ volatile("sync" ::: "memory");
}

static inline void cache_disable(void) {
    /* No portable Allegrex "disable" needed for the loader lifecycle; flushing
     * before program handoff is sufficient. */
    __asm__ volatile("sync" ::: "memory");
}

#endif /* KOSLOAD_PSP_CACHE_H */
