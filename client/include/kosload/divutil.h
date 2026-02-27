/* divutil.h — Division by compile-time constant without libgcc.
 *
 * SH4 lacks hardware divide; any / or % by a non-power-of-2 pulls
 * in __sdivsi3_i4i (~2 KB). These macros replace division with
 * reciprocal multiplication using the SH4's dmulu.l instruction.
 *
 * Limitations:
 *   - d must be a compile-time constant in [2, 65535]
 *   - n must be an unsigned 32-bit value
 *   - Powers of 2 should use >> instead
 */

#ifndef KOSLOAD_DIVUTIL_H
#define KOSLOAD_DIVUTIL_H

#include <stdint.h>

#define UDIV_CONST(n, d) \
    ((uint32_t)(((uint64_t)(uint32_t)(n) * (0xFFFFFFFFU / (uint32_t)(d) + 1U)) >> 32))

#define UMOD_CONST(n, d) \
    ((uint32_t)(n) - UDIV_CONST(n, d) * (uint32_t)(d))

#endif /* KOSLOAD_DIVUTIL_H */
