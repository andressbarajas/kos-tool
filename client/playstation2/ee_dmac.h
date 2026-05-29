/* client/playstation2/ee_dmac.h
 *
 * EE DMAC register accessors + bit definitions, shared across the PS2
 * loader (SIF transport, GIF/video, IOP bootstrap). Addresses come from
 * ps2_memory_map.h; this header adds the volatile MMIO accessors and the
 * control/status/CHCR bit fields.
 *
 * Channels used by the loader (Sony channel numbering):
 *   ch2 = GIF (video), ch5 = SIF0 (IOP->EE), ch6 = SIF1 (EE->IOP).
 *
 * This is EE-side only — the IOP has a separate DMA controller at
 * 0xBF80xxxx that is NOT covered here.
 */

#ifndef __PS2_EE_DMAC_H
#define __PS2_EE_DMAC_H

#include <stdint.h>
#include "ps2_memory_map.h"

/* === Global DMAC registers =========================================== */

#define D_CTRL    (*(volatile uint32_t *)PS2_EE_REG_DMAC_CTRL)
#define D_STAT    (*(volatile uint32_t *)PS2_EE_REG_DMAC_STAT)
#define D_PCR     (*(volatile uint32_t *)PS2_EE_REG_DMAC_PCR)
#define D_SQWC    (*(volatile uint32_t *)PS2_EE_REG_DMAC_SQWC)
#define D_ENABLER (*(volatile uint32_t *)PS2_EE_REG_DMAC_ENABLER)
#define D_ENABLEW (*(volatile uint32_t *)PS2_EE_REG_DMAC_ENABLEW)

/* === Per-channel registers =========================================== */

/* ch2 — GIF */
#define D2_CHCR  (*(volatile uint32_t *)(PS2_EE_REG_DMAC_GIF_BASE + PS2_EE_DMAC_CHCR_OFFSET))
#define D2_MADR  (*(volatile uint32_t *)(PS2_EE_REG_DMAC_GIF_BASE + PS2_EE_DMAC_MADR_OFFSET))
#define D2_QWC   (*(volatile uint32_t *)(PS2_EE_REG_DMAC_GIF_BASE + PS2_EE_DMAC_QWC_OFFSET))

/* ch5 — SIF0 (IOP -> EE) */
#define D5_CHCR  (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF0_CHCR)
#define D5_QWC   (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF0_QWC)

/* ch6 — SIF1 (EE -> IOP) */
#define D6_CHCR  (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF1_CHCR)
#define D6_QWC   (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF1_QWC)
#define D6_TADR  (*(volatile uint32_t *)PS2_EE_REG_DMAC_SIF1_TADR)

/* === Bit fields ====================================================== */

/* Dn_CHCR */
#define CHCR_MOD_CHAIN     0x004
#define CHCR_TIE           0x080
#define CHCR_STR           0x100
#define SIF0_DCHAIN_CHCR   (CHCR_STR | CHCR_TIE | CHCR_MOD_CHAIN)   /* Receive DMA chain from IOP. */
#define SIF1_SOURCE_CHCR   (CHCR_STR | CHCR_TIE | CHCR_MOD_CHAIN)   /* Send DMA chain to IOP. */

/* D_CTRL */
#define D_CTRL_DMAE     0x00000001   /* global DMA enable */

/* D_PCR channel DMA enable (CDE) */
#define D_PCR_CDE_GIF   0x00040000   /* ch2 */
#define D_PCR_CDE_SIF0  0x00200000   /* ch5 */
#define D_PCR_CDE_SIF1  0x00400000   /* ch6 */

/* D_STAT channel interrupt-status bits (lower half; write-1-to-clear) */
#define D_STAT_GIF      0x00000004   /* ch2 */
#define D_STAT_SIF0     0x00000020   /* ch5 */
#define D_STAT_SIF1     0x00000040   /* ch6 */
#define D_STAT_SIF      (D_STAT_SIF0 | D_STAT_SIF1)

/* D_ENABLER / D_ENABLEW */
#define DMAC_CPND       0x00010000   /* global channel-pending hold */

#endif /* __PS2_EE_DMAC_H */
