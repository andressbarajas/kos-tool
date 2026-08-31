/* divutil.h — Integer division by a compile-time constant, without calling a
 * software-division routine.
 *
 * Division by a constant d is replaced by a multiply by the fixed-point
 * reciprocal ceil(2^32 / d), keeping the high half of the 32x32 -> 64 product.
 * That is a widening multiply-high, which every target we build for has in
 * hardware, so no libgcc divide is linked.
 *
 * Requirements:
 *   - d is a compile-time constant >= 2 (d == 1 overflows the reciprocal)
 *   - n is an unsigned 32-bit value
 *   - powers of two should use >> instead: exact, and cheaper still
 */

#ifndef KOSLOAD_DIVUTIL_H
#define KOSLOAD_DIVUTIL_H

#include <stdint.h>

/* Fixed-point reciprocal of d, rounded up: ceil(2^32 / d). */
#define UDIV_RECIP(d) (0xFFFFFFFFU / (uint32_t)(d) + 1U)

/* n divided by d.
 *
 * The stored reciprocal is rounded up, so it is always a touch larger than
 * 1/d.  Multiplying by it therefore adds a little surplus on top of n/d -- but
 * that surplus is always less than 1, whatever n and d are.  So the answer is
 * never too small, and at worst one too large: either floor(n/d), or
 * floor(n/d) + 1 when n/d sits close enough below a whole number for the
 * surplus to tip it over.
 *
 * When d divides 2^32 evenly there is no rounding, no surplus, and the answer
 * is always exact. */
#define UDIV_CONST(n, d) \
    ((uint32_t)(((uint64_t)(uint32_t)(n) * UDIV_RECIP(d)) >> 32))

/* What is left over after that division: n - UDIV_CONST(n, d) * d.
 *
 * CAUTION: this is not a general remainder.  It only works where the quotient
 * above is exact.  If the quotient came out one too large, this subtracts one
 * d too many; the true answer is negative, and unsigned math turns that into a
 * huge number just under 2^32 rather than a small one.
 *
 * Always safe when d divides 2^32 evenly.  Otherwise check for that wrap and
 * fix up both halves, the way ms_to_ticks() does below. */
#define UMOD_CONST(n, d) \
    ((uint32_t)(n) - UDIV_CONST(n, d) * (uint32_t)(d))

/* Milliseconds -> ticks, at a rate of tps ticks per second, using only 32-bit
 * arithmetic.
 *
 * The obvious way, tps * ms / 1000, needs 64 bits: the product alone overflows
 * a uint32 as soon as tps goes past 2^32 / ms.
 *
 * Dividing first instead -- ticks per millisecond, times ms -- stays in 32
 * bits, but tps / 1000 drops its remainder, losing up to 999 of the ticks in
 * every second.  At a fast tick rate that is lost in the noise; at a slow one
 * it is a few percent off.
 *
 * So keep both pieces: multiply the whole part by ms, then scale the leftover
 * and add it back.  The answer comes out exact, give or take the single tick
 * each UDIV_CONST may round up.
 *
 * Good for any result that fits in 32 bits, i.e. tps * ms < 1000 * 2^32.
 */
static inline uint32_t ms_to_ticks(uint32_t tps, uint32_t ms) {
    uint32_t per_ms = UDIV_CONST(tps, 1000);
    uint32_t rem    = tps - per_ms * 1000;

    /* per_ms overshot by one, so the subtraction wrapped and rem landed just
     * under 2^32.  Undo both. */
    if(rem >= 1000u) {
        per_ms--;
        rem += 1000u;
    }

    return per_ms * ms + UDIV_CONST(rem * ms, 1000);
}

#endif /* KOSLOAD_DIVUTIL_H */
