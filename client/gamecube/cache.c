/* client/gamecube/cache.c */
/*
 * GameCube cache management.
 *
 * Uses PowerPC 750 cache block instructions to flush and invalidate
 * the L1 data and instruction caches.
 */

#include "cache.h"

#define CACHE_LINE_SIZE 32

void cache_flush_dc(const void *addr, uint32_t size) {
    uint32_t start = (uint32_t)addr & ~(CACHE_LINE_SIZE - 1);
    uint32_t end = ((uint32_t)addr + size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    uint32_t p;

    for(p = start; p < end; p += CACHE_LINE_SIZE)
        __asm__ volatile("dcbf 0,%0" ::"r"(p));

    __asm__ volatile("sync");
}

void cache_invalidate_ic(const void *addr, uint32_t size) {
    uint32_t start = (uint32_t)addr & ~(CACHE_LINE_SIZE - 1);
    uint32_t end = ((uint32_t)addr + size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    uint32_t p;

    for(p = start; p < end; p += CACHE_LINE_SIZE)
        __asm__ volatile("icbi 0,%0" ::"r"(p));

    __asm__ volatile("sync; isync");
}

void cache_flush_range(const void *addr, uint32_t size) {
    cache_flush_dc(addr, size);
    cache_invalidate_ic(addr, size);
}

void cache_disable(void) {
    uint32_t hid0;

    /* Read HID0 */
    __asm__ volatile("mfspr %0, 1008" : "=r"(hid0));

    /* Flush all dirty D-cache lines by flash invalidating with flush */
    hid0 |= HID0_DCFI | HID0_ICFI;
    __asm__ volatile("mtspr 1008, %0" ::"r"(hid0));
    __asm__ volatile("sync; isync");

    /* Disable both caches */
    hid0 &= ~(HID0_ICE | HID0_DCE | HID0_DCFI | HID0_ICFI);
    __asm__ volatile("mtspr 1008, %0" ::"r"(hid0));
    __asm__ volatile("sync; isync");
}

void cache_enable(void) {
    uint32_t hid0;

    /* Read HID0 */
    __asm__ volatile("mfspr %0, 1008" : "=r"(hid0));

    /* Flash invalidate both caches first */
    hid0 |= HID0_ICE | HID0_DCE | HID0_ICFI | HID0_DCFI;
    __asm__ volatile("mtspr 1008, %0" ::"r"(hid0));
    __asm__ volatile("sync; isync");

    /* Clear flash invalidate bits */
    hid0 &= ~(HID0_ICFI | HID0_DCFI);
    __asm__ volatile("mtspr 1008, %0" ::"r"(hid0));
    __asm__ volatile("sync; isync");
}
