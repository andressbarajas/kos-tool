/* client/playstation2/ee_cop0.h
 *
 * Cleanroom EE COP0 register, exception, TLB, timer, and cache constants.
 */

#ifndef KOSLOAD_PS2_EE_COP0_H
#define KOSLOAD_PS2_EE_COP0_H

#include <stdint.h>

/* COP0 register numbers. */
#define EE_COP0_INDEX    0
#define EE_COP0_RANDOM   1
#define EE_COP0_ENTRYLO0 2
#define EE_COP0_ENTRYLO1 3
#define EE_COP0_CONTEXT  4
#define EE_COP0_PAGEMASK 5
#define EE_COP0_WIRED    6
#define EE_COP0_BADVADDR 8
#define EE_COP0_COUNT    9
#define EE_COP0_ENTRYHI  10
#define EE_COP0_COMPARE  11
#define EE_COP0_STATUS   12
#define EE_COP0_CAUSE    13
#define EE_COP0_EPC      14
#define EE_COP0_PRID     15
#define EE_COP0_CONFIG   16
#define EE_COP0_BADPADDR 23
#define EE_COP0_DEBUG    24
#define EE_COP0_PERF     25
#define EE_COP0_TAGLO    28
#define EE_COP0_TAGHI    29
#define EE_COP0_ERROREPC 30

/* Normal and bootstrap exception vectors. */
#define EE_COP0_VEC_RESET              0xBFC00000
#define EE_COP0_VEC_TLB_REFILL         0x80000000
#define EE_COP0_VEC_PERF_COUNTER       0x80000080
#define EE_COP0_VEC_DEBUG              0x80000100
#define EE_COP0_VEC_COMMON             0x80000180
#define EE_COP0_VEC_INTERRUPT          0x80000200
#define EE_COP0_VEC_BOOT_TLB_REFILL    0xBFC00200
#define EE_COP0_VEC_BOOT_PERF_COUNTER  0xBFC00280
#define EE_COP0_VEC_BOOT_DEBUG         0xBFC00300
#define EE_COP0_VEC_BOOT_COMMON        0xBFC00380
#define EE_COP0_VEC_BOOT_INTERRUPT     0xBFC00400

/* COP0 Status ($12). */
#define EE_COP0_STATUS_IE              0x00000001
#define EE_COP0_STATUS_EXL             0x00000002
#define EE_COP0_STATUS_ERL             0x00000004
#define EE_COP0_STATUS_KSU_SHIFT       3
#define EE_COP0_STATUS_KSU_MASK        0x00000018
#define EE_COP0_STATUS_KSU_KERNEL      (0 << EE_COP0_STATUS_KSU_SHIFT)
#define EE_COP0_STATUS_KSU_SUPERVISOR  (1 << EE_COP0_STATUS_KSU_SHIFT)
#define EE_COP0_STATUS_KSU_USER        (2 << EE_COP0_STATUS_KSU_SHIFT)
#define EE_COP0_STATUS_INT0_INTC       0x00000400
#define EE_COP0_STATUS_INT1_DMAC       0x00000800
#define EE_COP0_STATUS_BEM             0x00001000
#define EE_COP0_STATUS_INT5_TIMER      0x00008000
#define EE_COP0_STATUS_EIE             0x00010000
#define EE_COP0_STATUS_EDI             0x00020000
#define EE_COP0_STATUS_CH              0x00040000
#define EE_COP0_STATUS_BEV             0x00400000
#define EE_COP0_STATUS_DEV             0x00800000
#define EE_COP0_STATUS_CU0             0x10000000
#define EE_COP0_STATUS_CU1             0x20000000
#define EE_COP0_STATUS_CU2             0x40000000
#define EE_COP0_STATUS_CU3             0x80000000
#define EE_COP0_STATUS_INTERRUPT_READY (EE_COP0_STATUS_IE | EE_COP0_STATUS_EIE)
#define EE_COP0_STATUS_LOADER_INIT                                                                           \
    (EE_COP0_STATUS_CU0 | EE_COP0_STATUS_CU1 | EE_COP0_STATUS_CU2 | EE_COP0_STATUS_EIE |                     \
     EE_COP0_STATUS_EDI | EE_COP0_STATUS_INT0_INTC | EE_COP0_STATUS_INT1_DMAC)

/* COP0 Cause ($13). */
#define EE_COP0_CAUSE_EXCCODE_SHIFT 2
#define EE_COP0_CAUSE_EXCCODE_MASK  0x0000007C
#define EE_COP0_CAUSE_INT0_INTC     0x00000400
#define EE_COP0_CAUSE_INT1_DMAC     0x00000800
#define EE_COP0_CAUSE_INT5_TIMER    0x00008000
#define EE_COP0_CAUSE_ERROR_SHIFT   16
#define EE_COP0_CAUSE_ERROR_MASK    0x00070000
#define EE_COP0_CAUSE_CE_SHIFT      28
#define EE_COP0_CAUSE_CE_MASK       0x30000000
#define EE_COP0_CAUSE_BD2           0x40000000
#define EE_COP0_CAUSE_BD            0x80000000
#define EE_COP0_CAUSE_EXCCODE(cause)                                                                         \
    (((uint32_t)(cause) & EE_COP0_CAUSE_EXCCODE_MASK) >> EE_COP0_CAUSE_EXCCODE_SHIFT)

#define EE_COP0_EXC_INTERRUPT        0x00
#define EE_COP0_EXC_TLB_MODIFIED     0x01
#define EE_COP0_EXC_TLB_LOAD_IFETCH  0x02
#define EE_COP0_EXC_TLB_STORE        0x03
#define EE_COP0_EXC_ADDR_LOAD_IFETCH 0x04
#define EE_COP0_EXC_ADDR_STORE       0x05
#define EE_COP0_EXC_BUS_INSTRUCTION  0x06
#define EE_COP0_EXC_BUS_DATA         0x07
#define EE_COP0_EXC_SYSCALL          0x08
#define EE_COP0_EXC_BREAKPOINT       0x09
#define EE_COP0_EXC_RESERVED_INSTR   0x0A
#define EE_COP0_EXC_COP_UNUSABLE     0x0B
#define EE_COP0_EXC_OVERFLOW         0x0C
#define EE_COP0_EXC_TRAP             0x0D

#define EE_COP0_ERROR_RESET          0x00
#define EE_COP0_ERROR_NMI            0x01
#define EE_COP0_ERROR_PERF_COUNTER   0x02
#define EE_COP0_ERROR_DEBUG          0x03

/* COP0 Config ($16), KSEG0 cache mode field. */
#define EE_COP0_CONFIG_K0_MASK       0x00000007
#define EE_COP0_CONFIG_K0_UNCACHED   0x00000002
#define EE_COP0_CONFIG_K0_CACHED     0x00000003

/* TLB fields. */
#define EE_COP0_TLB_INDEX_MASK     0x0000003F
#define EE_COP0_TLB_RANDOM_MASK    0x0000003F
#define EE_COP0_TLB_WIRED_MASK     0x0000003F

#define EE_COP0_ENTRYLO_G            0x00000001
#define EE_COP0_ENTRYLO_V            0x00000002
#define EE_COP0_ENTRYLO_D            0x00000004
#define EE_COP0_ENTRYLO_C_SHIFT      3
#define EE_COP0_ENTRYLO_C_MASK       0x00000038
#define EE_COP0_ENTRYLO_C_UNCACHED   (2 << EE_COP0_ENTRYLO_C_SHIFT)
#define EE_COP0_ENTRYLO_C_CACHED     (3 << EE_COP0_ENTRYLO_C_SHIFT)
#define EE_COP0_ENTRYLO_C_ACCEL      (7 << EE_COP0_ENTRYLO_C_SHIFT)
#define EE_COP0_ENTRYLO_PFN_SHIFT    6
#define EE_COP0_ENTRYLO_PFN_MASK     0x03FFFFC0
#define EE_COP0_ENTRYLO_SCRATCHPAD   0x80000000

#define EE_COP0_PAGEMASK_MASK   0x01FFE000
#define EE_COP0_PAGEMASK_4K     (0x000 << 13)
#define EE_COP0_PAGEMASK_16K    (0x003 << 13)
#define EE_COP0_PAGEMASK_64K    (0x00F << 13)
#define EE_COP0_PAGEMASK_256K   (0x03F << 13)
#define EE_COP0_PAGEMASK_1M     (0x0FF << 13)
#define EE_COP0_PAGEMASK_4M     (0x3FF << 13)
#define EE_COP0_PAGEMASK_16M    (0xFFF << 13)

#define EE_COP0_ENTRYHI_ASID_MASK   0x000000FF
#define EE_COP0_ENTRYHI_VPN2_MASK   0xFFFFE000

/* EE cache geometry and relevant cache/tag constants. */
#define EE_COP0_ICACHE_SIZE      (16 * 1024)
#define EE_COP0_ICACHE_ASSOC     2
#define EE_COP0_ICACHE_LINE_SIZE 64
#define EE_COP0_ICACHE_SETS      (EE_COP0_ICACHE_SIZE / (EE_COP0_ICACHE_ASSOC * EE_COP0_ICACHE_LINE_SIZE))
#define EE_COP0_DCACHE_SIZE      (8 * 1024)
#define EE_COP0_DCACHE_ASSOC     2
#define EE_COP0_DCACHE_LINE_SIZE 64
#define EE_COP0_DCACHE_SETS      (EE_COP0_DCACHE_SIZE / (EE_COP0_DCACHE_ASSOC * EE_COP0_DCACHE_LINE_SIZE))

#define EE_COP0_CACHE_OP_DXLTG     0x10
#define EE_COP0_CACHE_OP_DXWBIN    0x14
#define EE_COP0_CACHE_OP_IHIN      0x0B
#define EE_COP0_CACHE_OP_DHWBIN    0x18
#define EE_COP0_CACHE_OP_DHIN      0x1A
#define EE_COP0_CACHE_OP_DHWOIN    0x1C

#define EE_COP0_TAGLO_PFN_MASK     0xFFFFF000
#define EE_COP0_TAGLO_DCACHE_DIRTY 0x00000040

static inline uint32_t ee_cop0_read_status(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $12" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_status(uint32_t value) {
    __asm__ volatile("mtc0 %0, $12\nsync.p" ::"r"(value) : "memory");
}

/* Suspend the broker ISR for the duration of a polling SIF op called from
 * guest context: clear Status.IE and return its prior value so it can be
 * restored. EI/DI only touch EIE (bit 16); IE (bit 0) needs an RMW. */
static inline uint32_t ee_cop0_save_clear_ie(void) {
    uint32_t s;
    __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    if(s & EE_COP0_STATUS_IE) {
        __asm__ volatile("mtc0 %0, $12\nsync.p" ::"r"(s & ~EE_COP0_STATUS_IE) : "memory");
        return EE_COP0_STATUS_IE;
    }
    return 0;
}

static inline void ee_cop0_restore_ie(uint32_t saved) {
    uint32_t s;
    if(!saved)
        return;
    __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    __asm__ volatile("mtc0 %0, $12\nsync.p" ::"r"(s | EE_COP0_STATUS_IE) : "memory");
}

static inline uint32_t ee_cop0_read_cause(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $13" : "=r"(value));
    return value;
}

static inline uint32_t ee_cop0_read_epc(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $14" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_epc(uint32_t value) {
    __asm__ volatile("mtc0 %0, $14\nsync.p" ::"r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_config(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $16" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_config(uint32_t value) {
    __asm__ volatile("mtc0 %0, $16\nsync.p" ::"r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_count(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $9" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_count(uint32_t value) {
    __asm__ volatile("mtc0 %0, $9\nsync.p" ::"r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_compare(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $11" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_compare(uint32_t value) {
    __asm__ volatile("mtc0 %0, $11\nsync.p" ::"r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_badvaddr(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $8" : "=r"(value));
    return value;
}

static inline uint32_t ee_cop0_read_badpaddr(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $23" : "=r"(value));
    return value;
}

static inline uint32_t ee_cop0_read_taglo(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $28" : "=r"(value));
    return value;
}

#endif /* KOSLOAD_PS2_EE_COP0_H */
