/* client/playstation2/cache.c */

#include "cache.h"

/*
 * D-cache geometry for the index sweep:
 *   8 KB total, 2-way set-associative, 64-byte lines -> 64 sets.
 *   vAddr[11:6] = set index, vAddr[0] = way (0 or 1).
 *   KSEG0 base (0x80000000) is identity-mapped and always safe to use
 *   as the address carrier; the actual writeback destination comes from
 *   the physical address stored in each line's tag, not from pAddr.
 */
#define DCACHE_SETS  (ARCH_CACHE_L1_DCACHE_SIZE / \
                      (ARCH_CACHE_L1_DCACHE_ASSOC * ARCH_CACHE_L1_DCACHE_LINESIZE))
#define DCACHE_KSEG0 ((uintptr_t)0x80000000)

void arch_dcache_purge_all(void) {

    __asm__ volatile("sync.l" ::: "memory");

    for(uintptr_t set = 0; set < DCACHE_SETS; set++) {
        uintptr_t addr = DCACHE_KSEG0 | (set << 6);

        __asm__ volatile(
            ".set push\n"
            ".set noreorder\n"
            "cache 0x14, 0(%0)\n"   /* DXWBIN way 0 */
            "sync.l\n"
            "cache 0x14, 1(%0)\n"   /* DXWBIN way 1 */
            "sync.l\n"
            ".set pop\n"
            :: "r"(addr) : "memory"
        );
    }
}

void arch_dcache_wback_all(void) {
    uint32_t taglo;

    /*
     * For each set/way: read the tag with DXLTG, check bit 6 (Dirty).
     * If dirty, reconstruct the KSEG0 address from the PFN stored in
     * TagLO[31:12] plus the set index in bits [11:6], then issue DHWOIN
     * (0x1c) to write back without invalidating.
     *
     * Address reconstruction:
     *   pAddr = (TagLO & 0xFFFFF000) | (set << 6)
     *   vAddr = 0x80000000 | pAddr          (KSEG0 identity map)
     *
     * NOP pairs after DXLTG guard the COP0 TagLO write-to-read hazard
     * before the MFC0, and after MFC0 guard its own read delay.
     */
    __asm__ volatile("sync.l" ::: "memory");

    for(uintptr_t set = 0; set < DCACHE_SETS; set++) {
        uintptr_t base = DCACHE_KSEG0 | (set << 6);
        uintptr_t vaddr;

        /* Way 0 */
        __asm__ volatile(
            ".set push\n"
            ".set noreorder\n"
            "cache 0x10, 0(%1)\n"   /* DXLTG: writes TagLO */
            "nop\n"
            "nop\n"
            "mfc0 %0, $28\n"        /* read TagLO */
            "nop\n"
            ".set pop\n"
            : "=r"(taglo) : "r"(base) : "memory"
        );

        if(taglo & (1 << 6)) {    /* Dirty bit */
            vaddr = 0x80000000 | (taglo & 0xFFFFF000) | (set << 6);
            __asm__ volatile(
                ".set push\n"
                ".set noreorder\n"
                "cache 0x1c, 0(%0)\n"   /* DHWOIN: writeback, keep valid */
                "sync.l\n"
                ".set pop\n"
                :: "r"(vaddr) : "memory"
            );
        }

        /* Way 1 */
        __asm__ volatile(
            ".set push\n"
            ".set noreorder\n"
            "cache 0x10, 1(%1)\n"   /* DXLTG: writes TagLO */
            "nop\n"
            "nop\n"
            "mfc0 %0, $28\n"        /* read TagLO */
            "nop\n"
            ".set pop\n"
            : "=r"(taglo) : "r"(base) : "memory"
        );

        if(taglo & (1 << 6)) {    /* Dirty bit */
            vaddr = 0x80000000 | (taglo & 0xFFFFF000) | (set << 6);
            __asm__ volatile(
                ".set push\n"
                ".set noreorder\n"
                "cache 0x1c, 0(%0)\n"   /* DHWOIN: writeback, keep valid */
                "sync.l\n"
                ".set pop\n"
                :: "r"(vaddr) : "memory"
            );
        }
    }
}

void arch_icache_inval_range(uintptr_t start, size_t count) {
    uintptr_t end = start + count;

    /* Align start to the beginning of a icache line */
    start &= ~(ARCH_CACHE_L1_ICACHE_LINESIZE - 1);

    /* IHIN requires the sequence to be directly preceded by SYNC.P. */
    __asm__ volatile("sync.p" ::: "memory");

    for(; start < end; start += ARCH_CACHE_L1_ICACHE_LINESIZE) {
        __asm__ volatile(
            ".set push\n"
            ".set noreorder\n"
            "cache 0x0b, 0(%0)\n"
            ".set pop\n"
            :: "r"(start) : "memory"
        );
    }

    /* Drain instruction prefetch so the CPU does not execute stale fetches. */
    __asm__ volatile("sync.p" ::: "memory");
}

void arch_icache_sync_range(uintptr_t start, size_t count) {
    arch_dcache_purge_range(start, count);
    arch_icache_inval_range(start, count);
}

void cache_flush_dc(const void *addr, size_t size) {
    arch_dcache_purge_range((uintptr_t)addr, size);
}

void cache_flush_range(const void *addr, size_t size) {
    arch_icache_sync_range((uintptr_t)addr, size);
}

void cache_invalidate_ic(const void *addr, size_t size) {
    arch_icache_inval_range((uintptr_t)addr, size);
}

void cache_disable(void) {
    uint32_t config;

    __asm__ volatile("mfc0 %0, $16" : "=r"(config));

    config = (config & ~COP0_CONFIG_K0_MASK) | COP0_CONFIG_K0_UNCACHED;

    __asm__ volatile(
        "mtc0 %0, $16\n"
        "sync.p\n"
        :: "r"(config)
    );
}

void cache_enable(void) {
    uint32_t config;

    __asm__ volatile("mfc0 %0, $16" : "=r"(config));

    config = (config & ~COP0_CONFIG_K0_MASK) | COP0_CONFIG_K0_WRITEBACK;

    __asm__ volatile(
        "mtc0 %0, $16\n"
        "sync.p\n"
        :: "r"(config)
    );
}
