/* client/wii/net/perfctr.h - PMCR compatibility using PPC time base. */
#ifndef KOSLOAD_WII_PERFCTR_H
#define KOSLOAD_WII_PERFCTR_H

#define PMCR_ELAPSED_TIME_MODE  0x23
#define PMCR_COUNT_CPU_CYCLES   0
#define PMCR_COUNT_RATIO_CYCLES 1

static inline void PMCR_Init(unsigned char which, unsigned char mode,
                             unsigned char count_type)
{
    (void)which; (void)mode; (void)count_type;
}

static inline void PMCR_Restart(unsigned char which, unsigned char mode,
                                unsigned char count_type)
{
    (void)which; (void)mode; (void)count_type;
}

static inline void PMCR_Read(unsigned char which,
                             volatile unsigned int *out_array)
{
    unsigned int tbl, tbu0, tbu1;
    (void)which;

    do {
        __asm__ volatile("mftbu %0" : "=r"(tbu0));
        __asm__ volatile("mftb  %0" : "=r"(tbl));
        __asm__ volatile("mftbu %0" : "=r"(tbu1));
    } while (tbu0 != tbu1);

    out_array[0] = tbl;
    out_array[1] = tbu0;
}

static inline void PMCR_Enable(unsigned char which, unsigned char mode,
                               unsigned char count_type,
                               unsigned char reset_counter)
{
    (void)which; (void)mode; (void)count_type; (void)reset_counter;
}

static inline void PMCR_Stop(unsigned char which) { (void)which; }
static inline void PMCR_Disable(unsigned char which) { (void)which; }

static inline unsigned short PMCR_Get_Config(unsigned char which)
{
    (void)which;
    return 0;
}

static inline unsigned long long int PMCR_RegRead(unsigned char which)
{
    volatile unsigned int arr[2];
    PMCR_Read(which, arr);
    return ((unsigned long long int)arr[1] << 32) | arr[0];
}

#endif /* KOSLOAD_WII_PERFCTR_H */
