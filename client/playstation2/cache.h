/* client/playstation2/cache.h
 *
 * EE (R5900) cache management.
 *
 * Ported from arch/dreamcast/include/arch/cache.h; adapted for MIPS R5900.
 * SH4-specific operations (store-queue writeback, movca.l alloc lines) and
 * the threshold-based *_all() helpers are omitted.
 *
 * R5900 cache instruction op codes (EE Core Instruction Set Manual, Ch. 4):
 *   0x0b = IHIN   — I-cache Hit Invalidate
 *   0x18 = DHWBIN — D-cache Hit Writeback Invalidate
 *   0x1a = DHIN   — D-cache Hit Invalidate (discards dirty data, no writeback)
 *   0x1c = DHWOIN — D-cache Hit Writeback Without Invalidate (writeback only)
 *
 * sync.l  — R5900 "synchronize loads/stores": drains the store buffer and
 *            ensures writes reach RAM before DMA can observe them.
 *            Required before sequences of DHWBIN, DHWOIN, or DHIN.
 *            DHWBIN and DHWOIN additionally require a directly-following
 *            sync.l; the helpers below include one after every hit op.
 * sync.p  — R5900 "synchronize pipeline": additionally drains the
 *            instruction prefetch buffer.
 *            Required before sequences of IHIN; also placed after to ensure
 *            the CPU does not execute stale prefetched instructions.
 */

#ifndef PS2_CACHE_H
#define PS2_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include "ee_cop0.h"

/* =========================================================================
 * EE (R5900) cache geometry
 *
 * I-cache: 16 KB, 2-way set-associative, 64-byte lines.
 * D-cache:  8 KB, 2-way set-associative, 64-byte lines.
 * No L2 cache.
 * =========================================================================*/
#define ARCH_CACHE_L1_ICACHE_SIZE       EE_COP0_ICACHE_SIZE
#define ARCH_CACHE_L1_ICACHE_ASSOC      EE_COP0_ICACHE_ASSOC
#define ARCH_CACHE_L1_ICACHE_LINESIZE   EE_COP0_ICACHE_LINE_SIZE

#define ARCH_CACHE_L1_DCACHE_SIZE       EE_COP0_DCACHE_SIZE
#define ARCH_CACHE_L1_DCACHE_ASSOC      EE_COP0_DCACHE_ASSOC
#define ARCH_CACHE_L1_DCACHE_LINESIZE   EE_COP0_DCACHE_LINE_SIZE

#define ARCH_CACHE_L2_CACHE_SIZE        0
#define ARCH_CACHE_L2_CACHE_ASSOC       0
#define ARCH_CACHE_L2_CACHE_LINESIZE    0

/* Legacy alias kept for existing callers. */
#define PS2_CACHE_LINE_SIZE  ARCH_CACHE_L1_DCACHE_LINESIZE

/* =========================================================================
 * D-cache whole-cache operations (non-inline; implemented in cache.c)
 *
 * arch_dcache_wback_all  — write back all dirty D-cache lines to RAM without
 *                          invalidating them.  Uses DXLTG (0x10) to read each
 *                          line's tag, checks TagLO[6] (Dirty bit), and issues
 *                          DHWOIN (0x1c) only on dirty lines, reconstructing
 *                          the KSEG0 address from TagLO[31:12] (PFN) + set index.
 * arch_dcache_purge_all  — write back all dirty D-cache lines and invalidate
 *                          the entire D-cache.  Uses DXWBIN (0x14).
 *
 * Address encoding for index ops: vAddr[11:6] = set index, vAddr[0] = way.
 * =========================================================================*/
void arch_dcache_wback_all(void);
void arch_dcache_purge_all(void);

/* =========================================================================
 * I-cache range operations (non-inline; implemented in cache.c)
 *
 * arch_icache_inval_range — invalidate I-cache lines covering [start, start+count).
 * arch_icache_sync_range  — writeback D-cache then invalidate I-cache for
 *                           the same range.  Use before executing newly
 *                           written or loaded code.
 * =========================================================================*/
void arch_icache_inval_range(uintptr_t start, size_t count);
void arch_icache_sync_range(uintptr_t start, size_t count);

/* =========================================================================
 * D-cache prefetch
 * =========================================================================*/
static inline void arch_dcache_pref_line(const void *src) {
    __builtin_prefetch(src);
}

/* =========================================================================
 * D-cache single-line operations (inline)
 *
 * Hit writeback operations include the required trailing SYNC.L so
 * callers cannot accidentally issue another cache op too soon.
 * =========================================================================*/

/* Write back a dirty D-cache line to RAM, leaving it valid in cache (DHWOIN). */
static inline void arch_dcache_wback_line(void *src) {
    __asm__ volatile(".set push\n"
                     ".set noreorder\n"
                     "sync.l\n"
                     "cache 0x1c, 0(%0)\n"
                     "sync.l\n"
                     ".set pop\n" ::"r"(src)
                     : "memory");
}

/* Write back a dirty D-cache line to RAM and invalidate it (DHWBIN). */
static inline void arch_dcache_purge_line(void *src) {
    __asm__ volatile(".set push\n"
                     ".set noreorder\n"
                     "sync.l\n"
                     "cache 0x18, 0(%0)\n"
                     "sync.l\n"
                     ".set pop\n" ::"r"(src)
                     : "memory");
}

/* Invalidate a D-cache line, discarding any dirty data without writeback (DHIN). */
static inline void arch_dcache_inval_line(void *src) {
    __asm__ volatile(".set push\n"
                     ".set noreorder\n"
                     "sync.l\n"
                     "cache 0x1a, 0(%0)\n"
                     "sync.l\n"
                     ".set pop\n" ::"r"(src)
                     : "memory");
}

/* =========================================================================
 * D-cache range operations (inline)
 *
 * sync.l is required before sequences of DHWBIN/DHWOIN/DHIN; DHWBIN
 * and DHWOIN also require a directly-following sync.l, which the
 * single-line helpers above include.
 * (EE Core Instruction Set Manual, Restrictions sections).
 * =========================================================================*/

/* Write back all dirty D-cache lines covering [start, start+count) (DHWOIN). */
static inline void arch_dcache_wback_range(uintptr_t start, size_t count) {
    if(count >= ARCH_CACHE_L1_DCACHE_SIZE * 2) { /* 16 KB */
        arch_dcache_wback_all();
        return;
    }

    uintptr_t end = start + count;

    /* Align start to the beginning of a dcache line */
    start &= ~(ARCH_CACHE_L1_DCACHE_LINESIZE - 1);

    __asm__ volatile("sync.l" ::: "memory");

    for(; start < end; start += ARCH_CACHE_L1_DCACHE_LINESIZE) {
        __asm__ volatile(".set push\n"
                         ".set noreorder\n"
                         "cache 0x1c, 0(%0)\n"
                         "sync.l\n"
                         ".set pop\n" ::"r"(start)
                         : "memory");
    }
}

/* Invalidate all D-cache lines covering [start, start+count).
 * Dirty data is discarded without writeback (DHIN). */
static inline void arch_dcache_inval_range(uintptr_t start, size_t count) {
    uintptr_t end = start + count;

    /* Align start to the beginning of a dcache line */
    start &= ~(ARCH_CACHE_L1_DCACHE_LINESIZE - 1);

    __asm__ volatile("sync.l" ::: "memory");

    for(; start < end; start += ARCH_CACHE_L1_DCACHE_LINESIZE) {
        __asm__ volatile(".set push\n"
                         ".set noreorder\n"
                         "cache 0x1a, 0(%0)\n"
                         "sync.l\n"
                         ".set pop\n" ::"r"(start)
                         : "memory");
    }
}

/* Write back and invalidate all D-cache lines covering [start, start+count) (DHWBIN). */
static inline void arch_dcache_purge_range(uintptr_t start, size_t count) {
    if(count >= ARCH_CACHE_L1_DCACHE_SIZE) { /* 8 KB */
        arch_dcache_purge_all();
        return;
    }

    uintptr_t end = start + count;

    /* Align start to the beginning of a dcache line */
    start &= ~(ARCH_CACHE_L1_DCACHE_LINESIZE - 1);

    __asm__ volatile("sync.l" ::: "memory");

    for(; start < end; start += ARCH_CACHE_L1_DCACHE_LINESIZE) {
        __asm__ volatile(".set push\n"
                         ".set noreorder\n"
                         "cache 0x18, 0(%0)\n"
                         "sync.l\n"
                         ".set pop\n" ::"r"(start)
                         : "memory");
    }
}

/* =========================================================================
 * Legacy API (inline wrappers around the arch_* functions above)
 *
 * Kept for callers already using these names throughout the codebase.
 * =========================================================================*/

/* Flush (writeback+invalidate) a D-cache range. */
void cache_flush_dc(const void *addr, size_t size);

/* Flush D-cache then invalidate I-cache for a range (use before executing
 * newly-written code). */
void cache_flush_range(const void *addr, size_t size);

/* Invalidate I-cache for a range. */
void cache_invalidate_ic(const void *addr, size_t size);

/* =========================================================================
 * COP0 Config cache mode control (non-inline; implemented in cache.c)
 *
 * The K0 field (bits [2:0]) of COP0 Config controls the cache mode for
 * KSEG0 accesses.
 * =========================================================================*/
#define COP0_CONFIG_K0_MASK      EE_COP0_CONFIG_K0_MASK      /* K0 field bit mask */
#define COP0_CONFIG_K0_UNCACHED  EE_COP0_CONFIG_K0_UNCACHED  /* K0 = 2: uncached */
#define COP0_CONFIG_K0_WRITEBACK EE_COP0_CONFIG_K0_CACHED    /* K0 = 3: write-back cached */

/* Disable caches: set COP0 Config K0 field to COP0_CONFIG_K0_UNCACHED. */
void cache_disable(void);

/* Enable caches: set COP0 Config K0 field to COP0_CONFIG_K0_WRITEBACK. */
void cache_enable(void);

#endif /* PS2_CACHE_H */
