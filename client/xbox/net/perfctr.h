/* client/xbox/net/perfctr.h — PMCR-compatible shim over the x86 TSC.
 *
 * The shared command layer (cmd_pmcr) and timing code expect the Dreamcast
 * PMCR_* API; on Xbox we back it with the CPU time-stamp counter (rdtsc).
 */
#ifndef __PERFCTR_H__
#define __PERFCTR_H__

#define PMCR_ELAPSED_TIME_MODE  0x23
#define PMCR_COUNT_CPU_CYCLES   0
#define PMCR_COUNT_RATIO_CYCLES 1
#define PMCR_DEFAULT_COUNT_TYPE PMCR_COUNT_CPU_CYCLES

/* Xbox CPU / TSC frequency (Pentium III @ 733.33 MHz). */
#define PS2_COUNT_FREQUENCY 733333333
#define PERFCOUNTER_SCALE   733333333

static inline unsigned long long int rdtsc64(void) {
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long int)hi << 32) | lo;
}

static inline void PMCR_Init(unsigned char which, unsigned char mode, unsigned char count_type) {
    (void)which; (void)mode; (void)count_type;
}
static inline void PMCR_Restart(unsigned char which, unsigned char mode, unsigned char count_type) {
    (void)which; (void)mode; (void)count_type;
}
static inline void PMCR_Read(unsigned char which, volatile unsigned int *out_array) {
    unsigned long long int t = rdtsc64();
    (void)which;
    out_array[0] = (unsigned int)t;
    out_array[1] = (unsigned int)(t >> 32);
}
static inline void PMCR_Enable(unsigned char which, unsigned char mode, unsigned char count_type,
                               unsigned char reset_counter) {
    (void)which; (void)mode; (void)count_type; (void)reset_counter;
}
static inline void PMCR_Stop(unsigned char which) { (void)which; }
static inline void PMCR_Disable(unsigned char which) { (void)which; }
static inline unsigned short PMCR_Get_Config(unsigned char which) { (void)which; return 0; }

static inline unsigned long long int PMCR_RegRead(unsigned char which) {
    (void)which;
    return rdtsc64();
}

static inline unsigned int PMCR_TicksToSeconds(unsigned long long int ticks) {
    return (unsigned int)(ticks / PERFCOUNTER_SCALE);
}

#endif /* __PERFCTR_H__ */
