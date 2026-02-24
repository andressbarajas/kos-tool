/* client/include/kosload/memfuncs.h */
/*
 * Platform-independent memory function interface.
 *
 * Each platform provides optimized implementations:
 *   DC: SH4-optimized assembly (memcpy.S, memfuncs.c)
 *   GC: standard library or PPC-optimized (memfuncs.c)
 *
 * Used by shared code (commands.c) instead of DC-specific names
 * like SH4_aligned_memcpy, to_p1, CacheBlockPurge.
 */

#ifndef KOSLOAD_MEMFUNCS_H
#define KOSLOAD_MEMFUNCS_H

/* Platform-optimized aligned memcpy (dest and src should be 4-byte aligned) */
void *fast_aligned_memcpy(void *dest, void *src, unsigned int numbytes);

/* Zero-fill memory (count is in 64-bit units, i.e. size_bytes / 8) */
void *fast_memset_zero_64(void *dest, unsigned int count_64bit);

/* Cache purge: writeback + invalidate (base 32-byte aligned, count in 32-byte blocks) */
void cache_block_purge(void *base, unsigned int count_32byte);

/* Map address to cached/fast-access region (DC: P1 mapping, GC: identity) */
void *to_cached(void *addr);

#endif /* KOSLOAD_MEMFUNCS_H */
