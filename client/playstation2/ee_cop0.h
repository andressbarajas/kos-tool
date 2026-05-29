/* client/playstation2/ee_cop0.h
 *
 * Cleanroom EE COP0 register, exception, TLB, timer, and cache constants.
 */

#ifndef KOSLOAD_PS2_EE_COP0_H
#define KOSLOAD_PS2_EE_COP0_H

#include <stdint.h>

/* COP0 register numbers. */
#define EE_COP0_INDEX       0u
#define EE_COP0_RANDOM      1u
#define EE_COP0_ENTRYLO0    2u
#define EE_COP0_ENTRYLO1    3u
#define EE_COP0_CONTEXT     4u
#define EE_COP0_PAGEMASK    5u
#define EE_COP0_WIRED       6u
#define EE_COP0_BADVADDR    8u
#define EE_COP0_COUNT       9u
#define EE_COP0_ENTRYHI     10u
#define EE_COP0_COMPARE     11u
#define EE_COP0_STATUS      12u
#define EE_COP0_CAUSE       13u
#define EE_COP0_EPC         14u
#define EE_COP0_PRID        15u
#define EE_COP0_CONFIG      16u
#define EE_COP0_BADPADDR    23u
#define EE_COP0_DEBUG       24u
#define EE_COP0_PERF        25u
#define EE_COP0_TAGLO       28u
#define EE_COP0_TAGHI       29u
#define EE_COP0_ERROREPC    30u

/* Normal and bootstrap exception vectors. */
#define EE_COP0_VEC_RESET              0xBFC00000u
#define EE_COP0_VEC_TLB_REFILL         0x80000000u
#define EE_COP0_VEC_PERF_COUNTER       0x80000080u
#define EE_COP0_VEC_DEBUG              0x80000100u
#define EE_COP0_VEC_COMMON             0x80000180u
#define EE_COP0_VEC_INTERRUPT          0x80000200u
#define EE_COP0_VEC_BOOT_TLB_REFILL    0xBFC00200u
#define EE_COP0_VEC_BOOT_PERF_COUNTER  0xBFC00280u
#define EE_COP0_VEC_BOOT_DEBUG         0xBFC00300u
#define EE_COP0_VEC_BOOT_COMMON        0xBFC00380u
#define EE_COP0_VEC_BOOT_INTERRUPT     0xBFC00400u

/* COP0 Status ($12). */
#define EE_COP0_STATUS_IE              0x00000001u
#define EE_COP0_STATUS_EXL             0x00000002u
#define EE_COP0_STATUS_ERL             0x00000004u
#define EE_COP0_STATUS_KSU_SHIFT       3u
#define EE_COP0_STATUS_KSU_MASK        0x00000018u
#define EE_COP0_STATUS_KSU_KERNEL      (0u << EE_COP0_STATUS_KSU_SHIFT)
#define EE_COP0_STATUS_KSU_SUPERVISOR  (1u << EE_COP0_STATUS_KSU_SHIFT)
#define EE_COP0_STATUS_KSU_USER        (2u << EE_COP0_STATUS_KSU_SHIFT)
#define EE_COP0_STATUS_INT0_INTC       0x00000400u
#define EE_COP0_STATUS_INT1_DMAC       0x00000800u
#define EE_COP0_STATUS_BEM             0x00001000u
#define EE_COP0_STATUS_INT5_TIMER      0x00008000u
#define EE_COP0_STATUS_EIE             0x00010000u
#define EE_COP0_STATUS_EDI             0x00020000u
#define EE_COP0_STATUS_CH              0x00040000u
#define EE_COP0_STATUS_BEV             0x00400000u
#define EE_COP0_STATUS_DEV             0x00800000u
#define EE_COP0_STATUS_CU0             0x10000000u
#define EE_COP0_STATUS_CU1             0x20000000u
#define EE_COP0_STATUS_CU2             0x40000000u
#define EE_COP0_STATUS_CU3             0x80000000u
#define EE_COP0_STATUS_INTERRUPT_READY \
    (EE_COP0_STATUS_IE | EE_COP0_STATUS_EIE)
#define EE_COP0_STATUS_LOADER_INIT \
    (EE_COP0_STATUS_CU0 | EE_COP0_STATUS_CU1 | EE_COP0_STATUS_CU2 | \
     EE_COP0_STATUS_EIE | EE_COP0_STATUS_EDI | \
     EE_COP0_STATUS_INT0_INTC | EE_COP0_STATUS_INT1_DMAC)

/* COP0 Cause ($13). */
#define EE_COP0_CAUSE_EXCCODE_SHIFT    2u
#define EE_COP0_CAUSE_EXCCODE_MASK     0x0000007Cu
#define EE_COP0_CAUSE_INT0_INTC        0x00000400u
#define EE_COP0_CAUSE_INT1_DMAC        0x00000800u
#define EE_COP0_CAUSE_INT5_TIMER       0x00008000u
#define EE_COP0_CAUSE_ERROR_SHIFT      16u
#define EE_COP0_CAUSE_ERROR_MASK       0x00070000u
#define EE_COP0_CAUSE_CE_SHIFT         28u
#define EE_COP0_CAUSE_CE_MASK          0x30000000u
#define EE_COP0_CAUSE_BD2              0x40000000u
#define EE_COP0_CAUSE_BD               0x80000000u
#define EE_COP0_CAUSE_EXCCODE(cause) \
    (((uint32_t)(cause) & EE_COP0_CAUSE_EXCCODE_MASK) >> EE_COP0_CAUSE_EXCCODE_SHIFT)

#define EE_COP0_EXC_INTERRUPT          0x00u
#define EE_COP0_EXC_TLB_MODIFIED       0x01u
#define EE_COP0_EXC_TLB_LOAD_IFETCH    0x02u
#define EE_COP0_EXC_TLB_STORE          0x03u
#define EE_COP0_EXC_ADDR_LOAD_IFETCH   0x04u
#define EE_COP0_EXC_ADDR_STORE         0x05u
#define EE_COP0_EXC_BUS_INSTRUCTION    0x06u
#define EE_COP0_EXC_BUS_DATA           0x07u
#define EE_COP0_EXC_SYSCALL            0x08u
#define EE_COP0_EXC_BREAKPOINT         0x09u
#define EE_COP0_EXC_RESERVED_INSTR     0x0Au
#define EE_COP0_EXC_COP_UNUSABLE       0x0Bu
#define EE_COP0_EXC_OVERFLOW           0x0Cu
#define EE_COP0_EXC_TRAP               0x0Du

#define EE_COP0_ERROR_RESET            0x00u
#define EE_COP0_ERROR_NMI              0x01u
#define EE_COP0_ERROR_PERF_COUNTER     0x02u
#define EE_COP0_ERROR_DEBUG            0x03u

/* COP0 Config ($16), KSEG0 cache mode field. */
#define EE_COP0_CONFIG_K0_MASK         0x00000007u
#define EE_COP0_CONFIG_K0_UNCACHED     0x00000002u
#define EE_COP0_CONFIG_K0_CACHED       0x00000003u

/* TLB fields. */
#define EE_COP0_TLB_INDEX_MASK         0x0000003Fu
#define EE_COP0_TLB_RANDOM_MASK        0x0000003Fu
#define EE_COP0_TLB_WIRED_MASK         0x0000003Fu

#define EE_COP0_ENTRYLO_G              0x00000001u
#define EE_COP0_ENTRYLO_V              0x00000002u
#define EE_COP0_ENTRYLO_D              0x00000004u
#define EE_COP0_ENTRYLO_C_SHIFT        3u
#define EE_COP0_ENTRYLO_C_MASK         0x00000038u
#define EE_COP0_ENTRYLO_C_UNCACHED     (2u << EE_COP0_ENTRYLO_C_SHIFT)
#define EE_COP0_ENTRYLO_C_CACHED       (3u << EE_COP0_ENTRYLO_C_SHIFT)
#define EE_COP0_ENTRYLO_C_ACCEL        (7u << EE_COP0_ENTRYLO_C_SHIFT)
#define EE_COP0_ENTRYLO_PFN_SHIFT      6u
#define EE_COP0_ENTRYLO_PFN_MASK       0x03FFFFC0u
#define EE_COP0_ENTRYLO_SCRATCHPAD     0x80000000u

#define EE_COP0_PAGEMASK_MASK          0x01FFE000u
#define EE_COP0_PAGEMASK_4K            (0x000u << 13)
#define EE_COP0_PAGEMASK_16K           (0x003u << 13)
#define EE_COP0_PAGEMASK_64K           (0x00Fu << 13)
#define EE_COP0_PAGEMASK_256K          (0x03Fu << 13)
#define EE_COP0_PAGEMASK_1M            (0x0FFu << 13)
#define EE_COP0_PAGEMASK_4M            (0x3FFu << 13)
#define EE_COP0_PAGEMASK_16M           (0xFFFu << 13)

#define EE_COP0_ENTRYHI_ASID_MASK      0x000000FFu
#define EE_COP0_ENTRYHI_VPN2_MASK      0xFFFFE000u

/* EE cache geometry and relevant cache/tag constants. */
#define EE_COP0_ICACHE_SIZE            (16u * 1024u)
#define EE_COP0_ICACHE_ASSOC           2u
#define EE_COP0_ICACHE_LINE_SIZE       64u
#define EE_COP0_ICACHE_SETS \
    (EE_COP0_ICACHE_SIZE / (EE_COP0_ICACHE_ASSOC * EE_COP0_ICACHE_LINE_SIZE))
#define EE_COP0_DCACHE_SIZE            (8u * 1024u)
#define EE_COP0_DCACHE_ASSOC           2u
#define EE_COP0_DCACHE_LINE_SIZE       64u
#define EE_COP0_DCACHE_SETS \
    (EE_COP0_DCACHE_SIZE / (EE_COP0_DCACHE_ASSOC * EE_COP0_DCACHE_LINE_SIZE))

#define EE_COP0_CACHE_OP_DXLTG         0x10u
#define EE_COP0_CACHE_OP_DXWBIN        0x14u
#define EE_COP0_CACHE_OP_IHIN          0x0Bu
#define EE_COP0_CACHE_OP_DHWBIN        0x18u
#define EE_COP0_CACHE_OP_DHIN          0x1Au
#define EE_COP0_CACHE_OP_DHWOIN        0x1Cu

#define EE_COP0_TAGLO_PFN_MASK         0xFFFFF000u
#define EE_COP0_TAGLO_DCACHE_DIRTY     0x00000040u

static inline uint32_t ee_cop0_read_status(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $12" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_status(uint32_t value)
{
    __asm__ volatile("mtc0 %0, $12\nsync.p" :: "r"(value) : "memory");
}

/* Suspend the broker ISR for the duration of a polling SIF op called from
 * guest context: clear Status.IE and return its prior value so it can be
 * restored. EI/DI only touch EIE (bit 16); IE (bit 0) needs an RMW. */
static inline uint32_t ee_cop0_save_clear_ie(void)
{
    uint32_t s;
    __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    if (s & EE_COP0_STATUS_IE) {
        __asm__ volatile("mtc0 %0, $12\nsync.p"
                         :: "r"(s & ~EE_COP0_STATUS_IE) : "memory");
        return EE_COP0_STATUS_IE;
    }
    return 0;
}

static inline void ee_cop0_restore_ie(uint32_t saved)
{
    uint32_t s;
    if (!saved) return;
    __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    __asm__ volatile("mtc0 %0, $12\nsync.p"
                     :: "r"(s | EE_COP0_STATUS_IE) : "memory");
}

static inline uint32_t ee_cop0_read_cause(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $13" : "=r"(value));
    return value;
}

static inline uint32_t ee_cop0_read_epc(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $14" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_epc(uint32_t value)
{
    __asm__ volatile("mtc0 %0, $14\nsync.p" :: "r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_config(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $16" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_config(uint32_t value)
{
    __asm__ volatile("mtc0 %0, $16\nsync.p" :: "r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_count(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $9" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_count(uint32_t value)
{
    __asm__ volatile("mtc0 %0, $9\nsync.p" :: "r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_compare(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $11" : "=r"(value));
    return value;
}

static inline void ee_cop0_write_compare(uint32_t value)
{
    __asm__ volatile("mtc0 %0, $11\nsync.p" :: "r"(value) : "memory");
}

static inline uint32_t ee_cop0_read_badvaddr(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $8" : "=r"(value));
    return value;
}

static inline uint32_t ee_cop0_read_badpaddr(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $23" : "=r"(value));
    return value;
}

static inline uint32_t ee_cop0_read_taglo(void)
{
    uint32_t value;
    __asm__ volatile("mfc0 %0, $28" : "=r"(value));
    return value;
}

#endif /* KOSLOAD_PS2_EE_COP0_H */
