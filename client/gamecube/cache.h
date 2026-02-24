/* client/gamecube/cache.h */
/*
 * GameCube cache management.
 *
 * PowerPC 750 (Gekko) L1 cache control using dcbf, icbi, sync, isync.
 * L1 data cache: 32KB, 32-byte lines.
 * L1 instruction cache: 32KB, 32-byte lines.
 */

#ifndef KOSLOAD_GC_CACHE_H
#define KOSLOAD_GC_CACHE_H

#include <stdint.h>

/* HID0 register bits for cache control */
#define HID0_ICE    (1 << 15)   /* I-cache enable */
#define HID0_DCE    (1 << 14)   /* D-cache enable */
#define HID0_ICFI   (1 << 11)   /* I-cache flash invalidate */
#define HID0_DCFI   (1 << 10)   /* D-cache flash invalidate */

/* Flush data cache for a memory range (write back dirty lines) */
void cache_flush_dc(const void *addr, uint32_t size);

/* Invalidate instruction cache for a memory range */
void cache_invalidate_ic(const void *addr, uint32_t size);

/* Flush data cache and invalidate instruction cache for a range.
 * Use this before executing newly loaded code. */
void cache_flush_range(const void *addr, uint32_t size);

/* Disable both L1 data and instruction caches.
 * Flushes all dirty data first. */
void cache_disable(void);

/* Enable both L1 data and instruction caches. */
void cache_enable(void);

#endif /* KOSLOAD_GC_CACHE_H */
