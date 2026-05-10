/* client/playstation2/net/perfctr.h */
/*
 * Performance counter compatibility header for PlayStation 2.
 * Provides PMCR-compatible functions using the MIPS COP0 Count register.
 *
 * The R5900 COP0 Count register runs at bus_clock/2 = 147.456MHz/2 = ~73.7MHz
 * (or 294.912MHz/4 depending on config). We use the same frequency as target.c.
 */

#ifndef __PERFCTR_H__
#define __PERFCTR_H__

/* Constants expected by shared code */
#define PMCR_ELAPSED_TIME_MODE      0x23
#define PMCR_COUNT_CPU_CYCLES       0
#define PMCR_COUNT_RATIO_CYCLES     1

/* PS2 COP0 Count frequency for elapsed time calculations */
#define PS2_COUNT_FREQUENCY         294912000

/* PMCR_Init: no-op on PS2 (COP0 Count is always running) */
static inline void PMCR_Init(unsigned char which, unsigned char mode,
                              unsigned char count_type)
{
    (void)which; (void)mode; (void)count_type;
}

/* PMCR_Restart: no-op on PS2 (COP0 Count cannot be reset) */
static inline void PMCR_Restart(unsigned char which, unsigned char mode,
                                 unsigned char count_type)
{
    (void)which; (void)mode; (void)count_type;
}

/* PMCR_Read: read COP0 Count into out_array[2].
 * out_array[0] = low 32 bits (Count), out_array[1] = 0 (no high word).
 * Same layout as DC PMCR for compatibility. */
static inline void PMCR_Read(unsigned char which,
                              volatile unsigned int *out_array)
{
    unsigned int count;
    (void)which;

    __asm__ volatile("mfc0 %0, $9" : "=r"(count));

    out_array[0] = count;  /* Low 32 bits */
    out_array[1] = 0;      /* No high word */
}

/* PMCR_Enable: no-op on PS2 */
static inline void PMCR_Enable(unsigned char which, unsigned char mode,
                                unsigned char count_type,
                                unsigned char reset_counter)
{
    (void)which; (void)mode; (void)count_type; (void)reset_counter;
}

/* PMCR_Stop / PMCR_Disable: no-ops */
static inline void PMCR_Stop(unsigned char which) { (void)which; }
static inline void PMCR_Disable(unsigned char which) { (void)which; }

/* PMCR_Get_Config: return dummy value */
static inline unsigned short PMCR_Get_Config(unsigned char which)
{
    (void)which;
    return 0;
}

/* PMCR_RegRead: return Count as 64-bit value */
static inline unsigned long long int PMCR_RegRead(unsigned char which)
{
    volatile unsigned int arr[2];
    PMCR_Read(which, arr);
    return ((unsigned long long int)arr[1] << 32) | arr[0];
}

/* Convert COP0 Count ticks to seconds.
 * Count frequency = 294912000 Hz.
 * Approximation: 1/294912000 ≈ 233/2^36 (error < 0.01%) */
static inline unsigned int PMCR_TicksToSeconds(unsigned long long int ticks)
{
    return (unsigned int)((ticks * 233ULL) >> 36);
}

/* Default count type */
#define PMCR_DEFAULT_COUNT_TYPE PMCR_COUNT_CPU_CYCLES

#endif /* __PERFCTR_H__ */
