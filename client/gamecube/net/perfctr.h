/* client/gamecube/net/perfctr.h */
/*
 * Performance counter compatibility header for GameCube.
 * Provides PMCR-compatible functions using the PPC Time Base Register.
 *
 * The PPC 750 (Gekko) TBR runs at bus_clock/4 = 162MHz/4 = 40.5MHz.
 * This gives ~24.7ns per tick, compared to SH4's ~5ns per tick.
 */

#ifndef __PERFCTR_H__
#define __PERFCTR_H__

/* Constants expected by shared code */
#define PMCR_ELAPSED_TIME_MODE      0x23
#define PMCR_COUNT_CPU_CYCLES       0
#define PMCR_COUNT_RATIO_CYCLES     1

/* GC TBR frequency for elapsed time calculations */
#define GC_TBR_FREQUENCY            40500000

/* PMCR_Init: no-op on GC (TBR is always running) */
static inline void PMCR_Init(unsigned char which, unsigned char mode,
                              unsigned char count_type)
{
    (void)which; (void)mode; (void)count_type;
}

/* PMCR_Restart: no-op on GC (TBR cannot be reset) */
static inline void PMCR_Restart(unsigned char which, unsigned char mode,
                                 unsigned char count_type)
{
    (void)which; (void)mode; (void)count_type;
}

/* PMCR_Read: read PPC Time Base Register into out_array[2].
 * out_array[0] = low 32 bits, out_array[1] = high 32 bits.
 * Same layout as DC PMCR for compatibility. */
static inline void PMCR_Read(unsigned char which,
                              volatile unsigned int *out_array)
{
    unsigned int tbl, tbu0, tbu1;
    (void)which;

    /* Read TBR with rollover protection */
    do {
        __asm__ volatile("mftbu %0" : "=r"(tbu0));
        __asm__ volatile("mftb  %0" : "=r"(tbl));
        __asm__ volatile("mftbu %0" : "=r"(tbu1));
    } while (tbu0 != tbu1);

    out_array[0] = tbl;    /* Low 32 bits */
    out_array[1] = tbu0;   /* High 32 bits */
}

/* PMCR_Enable: no-op on GC (TBR cannot be configured) */
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

/* PMCR_RegRead: return TBR as 64-bit value */
static inline unsigned long long int PMCR_RegRead(unsigned char which)
{
    volatile unsigned int arr[2];
    PMCR_Read(which, arr);
    return ((unsigned long long int)arr[1] << 32) | arr[0];
}

/* Convert TBR ticks to seconds.
 * TBR frequency = 40.5MHz.
 * Approximation: 1/40500000 ≈ 3394/2^37 (error < 0.01%) */
static inline unsigned int PMCR_TicksToSeconds(unsigned long long int ticks)
{
    return (unsigned int)((ticks * 3394ULL) >> 37);
}

/* Default count type — irrelevant on GC (PMCR_Init is a no-op) */
#define PMCR_DEFAULT_COUNT_TYPE PMCR_COUNT_CPU_CYCLES

#endif /* __PERFCTR_H__ */
