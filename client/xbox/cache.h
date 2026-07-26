/* client/xbox/cache.h — x86 cache maintenance for the Xbox loader.
 *
 * The Pentium III has hardware-coherent, bus-snooped caches and the nForce
 * NIC's DMA is cache-coherent, so the DC/GC/PS2-style explicit flush/invalidate
 * helpers reduce to a full write-back (wbinvd) used only when disabling caches
 * before handing control to a loaded program.
 */

#ifndef KOSLOAD_XBOX_CACHE_H
#define KOSLOAD_XBOX_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* Write back and invalidate the entire cache (ring-0 wbinvd). */
static inline void cache_wbinvd(void) {
    __asm__ volatile("wbinvd" ::: "memory");
}

/* Flush a range so a loaded program sees freshly written code/data.  On x86
 * the caches are coherent for instruction fetch after a write on the same
 * core once serialized, so a full wbinvd is sufficient and simplest. */
static inline void cache_flush_range(const void *addr, size_t size) {
    (void)addr;
    (void)size;
    cache_wbinvd();
}

/* Disable the data/instruction caches (CR0.CD=1, then wbinvd).  Rarely needed;
 * provided for parity with the other ports' disable_cache hook. */
static inline void cache_disable(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1u << 30); /* CD = cache disable */
    cr0 &= ~(1u << 29); /* NW = 0 */
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    cache_wbinvd();
}

#endif /* KOSLOAD_XBOX_CACHE_H */
