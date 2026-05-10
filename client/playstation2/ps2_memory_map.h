/* client/playstation2/ps2_memory_map.h
 *
 * Common PlayStation 2 EE/IOP memory-map constants.
 *
 * This header contains addresses and simple address-conversion helpers only.
 * It is intentionally usable from both EE and cleanroom IOP translation units:
 * no libc headers, no inline code, and no SDK-owned names.
 *
 * Verification notes live in AGENT/ps2-memory-map.md.
 */

#ifndef KOSLOAD_PS2_MEMORY_MAP_H
#define KOSLOAD_PS2_MEMORY_MAP_H

/* ------------------------------------------------------------------------- */
/* EE virtual segments and RAM aliases.                                      */
/* ------------------------------------------------------------------------- */

#define PS2_EE_KUSEG_BASE                0x00000000u
#define PS2_EE_KUSEG_END                 0x7FFFFFFFu
#define PS2_EE_KSEG0_BASE                0x80000000u
#define PS2_EE_KSEG0_END                 0x9FFFFFFFu
#define PS2_EE_KSEG1_BASE                0xA0000000u
#define PS2_EE_KSEG1_END                 0xBFFFFFFFu
#define PS2_EE_KSSEG_BASE                0xC0000000u
#define PS2_EE_KSSEG_END                 0xDFFFFFFFu
#define PS2_EE_KSEG3_BASE                0xE0000000u
#define PS2_EE_KSEG3_END                 0xFFFFFFFFu

#define PS2_EE_MAIN_RAM_PHYS_BASE        0x00000000u
#define PS2_EE_MAIN_RAM_SIZE             0x02000000u
#define PS2_EE_KERNEL_RESERVED_SIZE      0x00100000u
#define PS2_EE_MAIN_RAM_CACHED_BASE      0x00000000u
#define PS2_EE_MAIN_RAM_UNCACHED_BASE    0x20000000u
#define PS2_EE_MAIN_RAM_ACCEL_BASE       0x30100000u
#define PS2_EE_MAIN_RAM_ACCEL_PHYS_BASE  0x00100000u
#define PS2_EE_MAIN_RAM_ACCEL_SIZE       0x01F00000u

#define PS2_EE_SCRATCHPAD_BASE           0x70000000u
#define PS2_EE_SCRATCHPAD_SIZE           0x00004000u

#define PS2_EE_BIOS_PHYS_BASE            0x1FC00000u
#define PS2_EE_BIOS_SIZE                 0x00400000u
#define PS2_EE_BIOS_CACHED_BASE          0x9FC00000u
#define PS2_EE_BIOS_UNCACHED_BASE        0xBFC00000u

#define PS2_EE_IOP_RAM_BASE              0x1C000000u
#define PS2_EE_IOP_RAM_UNCACHED_BASE     0xBC000000u

#define PS2_EE_PHYS(addr) \
    ((unsigned int)(addr) & 0x1FFFFFFFu)
#define PS2_EE_KSEG0_ADDR(addr) \
    (PS2_EE_KSEG0_BASE | PS2_EE_PHYS(addr))
#define PS2_EE_KSEG1_ADDR(addr) \
    (PS2_EE_KSEG1_BASE | PS2_EE_PHYS(addr))

/* ------------------------------------------------------------------------- */
/* EE hardware registers.  These are KSEG1, uncached addresses for C code.    */
/* ------------------------------------------------------------------------- */

#define PS2_EE_REG_IO_PHYS_BASE          0x10000000u
#define PS2_EE_REG_IO_BASE               0xB0000000u

#define PS2_EE_REG_TIMER0_BASE           0xB0000000u
#define PS2_EE_REG_TIMER1_BASE           0xB0000800u
#define PS2_EE_REG_TIMER2_BASE           0xB0001000u
#define PS2_EE_REG_TIMER3_BASE           0xB0001800u
#define PS2_EE_TIMER_COUNT_OFFSET        0x00u
#define PS2_EE_TIMER_MODE_OFFSET         0x10u
#define PS2_EE_TIMER_COMP_OFFSET         0x20u
#define PS2_EE_TIMER_HOLD_OFFSET         0x30u

#define PS2_EE_REG_IPU_CMD               0xB0002000u
#define PS2_EE_REG_IPU_CTRL              0xB0002010u
#define PS2_EE_REG_IPU_BP                0xB0002020u
#define PS2_EE_REG_IPU_TOP               0xB0002030u
#define PS2_EE_REG_IPU_OUT_FIFO          0xB0007000u
#define PS2_EE_REG_IPU_IN_FIFO           0xB0007010u

#define PS2_EE_REG_GIF_CTRL              0xB0003000u
#define PS2_EE_REG_GIF_MODE              0xB0003010u
#define PS2_EE_REG_GIF_STAT              0xB0003020u
#define PS2_EE_REG_GIF_TAG0              0xB0003040u
#define PS2_EE_REG_GIF_TAG1              0xB0003050u
#define PS2_EE_REG_GIF_TAG2              0xB0003060u
#define PS2_EE_REG_GIF_TAG3              0xB0003070u
#define PS2_EE_REG_GIF_CNT               0xB0003080u
#define PS2_EE_REG_GIF_P3CNT             0xB0003090u
#define PS2_EE_REG_GIF_P3TAG             0xB00030A0u
#define PS2_EE_REG_GIF_FIFO              0xB0006000u

#define PS2_EE_REG_DMAC_VIF0_BASE        0xB0008000u
#define PS2_EE_REG_DMAC_VIF1_BASE        0xB0009000u
#define PS2_EE_REG_DMAC_GIF_BASE         0xB000A000u
#define PS2_EE_REG_DMAC_IPU_FROM_BASE    0xB000B000u
#define PS2_EE_REG_DMAC_IPU_TO_BASE      0xB000B400u
#define PS2_EE_REG_DMAC_SIF0_BASE        0xB000C000u
#define PS2_EE_REG_DMAC_SIF1_BASE        0xB000C400u
#define PS2_EE_REG_DMAC_SIF2_BASE        0xB000C800u
#define PS2_EE_REG_DMAC_SPR_FROM_BASE    0xB000D000u
#define PS2_EE_REG_DMAC_SPR_TO_BASE      0xB000D400u

#define PS2_EE_DMAC_CHCR_OFFSET          0x00u
#define PS2_EE_DMAC_MADR_OFFSET          0x10u
#define PS2_EE_DMAC_QWC_OFFSET           0x20u
#define PS2_EE_DMAC_TADR_OFFSET          0x30u
#define PS2_EE_DMAC_ASR0_OFFSET          0x40u
#define PS2_EE_DMAC_ASR1_OFFSET          0x50u
#define PS2_EE_DMAC_SADR_OFFSET          0x80u

#define PS2_EE_REG_DMAC_SIF0_CHCR        0xB000C000u
#define PS2_EE_REG_DMAC_SIF0_QWC         0xB000C020u
#define PS2_EE_REG_DMAC_SIF1_CHCR        0xB000C400u
#define PS2_EE_REG_DMAC_SIF1_QWC         0xB000C420u
#define PS2_EE_REG_DMAC_SIF1_TADR        0xB000C430u

#define PS2_EE_REG_DMAC_CTRL             0xB000E000u
#define PS2_EE_REG_DMAC_STAT             0xB000E010u
#define PS2_EE_REG_DMAC_PCR              0xB000E020u
#define PS2_EE_REG_DMAC_SQWC             0xB000E030u
#define PS2_EE_REG_DMAC_RBSR             0xB000E040u
#define PS2_EE_REG_DMAC_RBOR             0xB000E050u
#define PS2_EE_REG_DMAC_STADR            0xB000E060u
#define PS2_EE_REG_DMAC_ENABLER          0xB000F520u
#define PS2_EE_REG_DMAC_ENABLEW          0xB000F590u

#define PS2_EE_REG_INTC_STAT             0xB000F000u
#define PS2_EE_REG_INTC_MASK             0xB000F010u

#define PS2_EE_REG_SIF_MSCOM             0xB000F200u
#define PS2_EE_REG_SIF_SMCOM             0xB000F210u
#define PS2_EE_REG_SIF_MSFLAG            0xB000F220u
#define PS2_EE_REG_SIF_SMFLAG            0xB000F230u
#define PS2_EE_REG_SIF_CTRL              0xB000F240u
#define PS2_EE_REG_SIF_BD6               0xB000F260u

#define PS2_EE_REG_KPUTCHAR              0xB000F180u
#define PS2_EE_REG_MCH_DRD               0xB000F430u
#define PS2_EE_REG_MCH_RICM              0xB000F440u

#define PS2_EE_REG_GS_PRIV_PHYS_BASE     0x12000000u
#define PS2_EE_REG_GS_PRIV_BASE          0xB2000000u
#define PS2_EE_REG_GS_PMODE              0xB2000000u
#define PS2_EE_REG_GS_SMODE1             0xB2000010u
#define PS2_EE_REG_GS_SMODE2             0xB2000020u
#define PS2_EE_REG_GS_SRFSH              0xB2000030u
#define PS2_EE_REG_GS_SYNCH1             0xB2000040u
#define PS2_EE_REG_GS_SYNCH2             0xB2000050u
#define PS2_EE_REG_GS_SYNCV              0xB2000060u
#define PS2_EE_REG_GS_DISPFB1            0xB2000070u
#define PS2_EE_REG_GS_DISPLAY1           0xB2000080u
#define PS2_EE_REG_GS_DISPFB2            0xB2000090u
#define PS2_EE_REG_GS_DISPLAY2           0xB20000A0u
#define PS2_EE_REG_GS_EXTBUF             0xB20000B0u
#define PS2_EE_REG_GS_EXTDATA            0xB20000C0u
#define PS2_EE_REG_GS_EXTWRITE           0xB20000D0u
#define PS2_EE_REG_GS_BGCOLOR            0xB20000E0u
#define PS2_EE_REG_GS_CSR                0xB2001000u
#define PS2_EE_REG_GS_IMR                0xB2001010u
#define PS2_EE_REG_GS_BUSDIR             0xB2001040u
#define PS2_EE_REG_GS_SIGLBLID           0xB2001080u

/* ------------------------------------------------------------------------- */
/* IOP virtual segments, memory, and hardware-register aliases.              */
/* ------------------------------------------------------------------------- */

#define PS2_IOP_KUSEG_BASE               0x00000000u
#define PS2_IOP_KUSEG_END                0x7FFFFFFFu
#define PS2_IOP_KSEG0_BASE               0x80000000u
#define PS2_IOP_KSEG0_END                0x9FFFFFFFu
#define PS2_IOP_KSEG1_BASE               0xA0000000u
#define PS2_IOP_KSEG1_END                0xBFFFFFFFu

#define PS2_IOP_MAIN_RAM_PHYS_BASE       0x00000000u
#define PS2_IOP_MAIN_RAM_SIZE            0x00200000u
#define PS2_IOP_BIOS_PHYS_BASE           0x1FC00000u
#define PS2_IOP_BIOS_SIZE                0x00400000u
#define PS2_IOP_BIOS_CACHED_BASE         0x9FC00000u
#define PS2_IOP_BIOS_UNCACHED_BASE       0xBFC00000u

#define PS2_IOP_REG_SIF_PHYS_BASE        0x1D000000u
#define PS2_IOP_REG_IO_PHYS_BASE         0x1F800000u
#define PS2_IOP_REG_IO_SIZE              0x00010000u
#define PS2_IOP_REG_SPU2_PHYS_BASE       0x1F900000u
#define PS2_IOP_REG_CACHE_CTRL_BASE      0xFFFE0000u

#define PS2_IOP_PHYS(addr) \
    ((unsigned int)(addr) & 0x1FFFFFFFu)
#define PS2_IOP_KSEG0_ADDR(addr) \
    (PS2_IOP_KSEG0_BASE | PS2_IOP_PHYS(addr))
#define PS2_IOP_KSEG1_ADDR(addr) \
    (PS2_IOP_KSEG1_BASE | PS2_IOP_PHYS(addr))

#define PS2_IOP_REG_SIF_BASE             0xBD000000u
#define PS2_IOP_REG_SIF_MSCOM            0xBD000000u
#define PS2_IOP_REG_SIF_SMCOM            0xBD000010u
#define PS2_IOP_REG_SIF_MSFLAG           0xBD000020u
#define PS2_IOP_REG_SIF_SMFLAG           0xBD000030u
#define PS2_IOP_REG_SIF_CTRL             0xBD000040u
#define PS2_IOP_REG_SIF_BD6              0xBD000060u

#define PS2_IOP_REG_CDVD_BASE            0xBF402000u
#define PS2_IOP_REG_CDVD_NCMD            0xBF402004u
#define PS2_IOP_REG_CDVD_NCMD_STAT       0xBF402005u
#define PS2_IOP_REG_CDVD_ERROR           0xBF402006u
#define PS2_IOP_REG_CDVD_BREAK           0xBF402007u
#define PS2_IOP_REG_CDVD_ISTAT           0xBF402008u
#define PS2_IOP_REG_CDVD_STATUS          0xBF40200Au
#define PS2_IOP_REG_CDVD_DISK_TYPE       0xBF40200Fu
#define PS2_IOP_REG_CDVD_SCMD            0xBF402016u
#define PS2_IOP_REG_CDVD_SCMD_STAT       0xBF402017u
#define PS2_IOP_REG_CDVD_SCMD_PARAMS     0xBF402018u

#define PS2_IOP_REG_INTC_STAT            0xBF801070u
#define PS2_IOP_REG_INTC_MASK            0xBF801074u
#define PS2_IOP_REG_INTC_CTRL            0xBF801078u

#define PS2_IOP_REG_DMA_OLD_CHANNEL_BASE 0xBF801080u
#define PS2_IOP_REG_DMA_NEW_CHANNEL_BASE 0xBF801500u
#define PS2_IOP_REG_DMA_DPCR             0xBF8010F0u
#define PS2_IOP_REG_DMA_DICR             0xBF8010F4u
#define PS2_IOP_REG_DMA_DPCR2            0xBF801570u
#define PS2_IOP_REG_DMA_DICR2            0xBF801574u
#define PS2_IOP_REG_DMA_DMACEN           0xBF801578u
#define PS2_IOP_REG_DMA_DMACINTEN        0xBF80157Cu
#define PS2_IOP_REG_DMA_DPCR3            0xBF8015F0u

#define PS2_IOP_REG_TIMER0_BASE          0xBF801100u
#define PS2_IOP_REG_TIMER1_BASE          0xBF801110u
#define PS2_IOP_REG_TIMER2_BASE          0xBF801120u
#define PS2_IOP_REG_TIMER3_BASE          0xBF801480u
#define PS2_IOP_REG_TIMER4_BASE          0xBF801490u
#define PS2_IOP_REG_TIMER5_BASE          0xBF8014A0u

#define PS2_IOP_REG_SIO2_BASE            0xBF808200u
#define PS2_IOP_REG_SIO2_SEND3_BASE      0xBF808200u
#define PS2_IOP_REG_SIO2_SEND12_BASE     0xBF808240u
#define PS2_IOP_REG_SIO2_IN_FIFO         0xBF808260u
#define PS2_IOP_REG_SIO2_OUT_FIFO        0xBF808264u
#define PS2_IOP_REG_SIO2_CTRL            0xBF808268u
#define PS2_IOP_REG_SIO2_RECV1           0xBF80826Cu
#define PS2_IOP_REG_SIO2_RECV2           0xBF808270u
#define PS2_IOP_REG_SIO2_RECV3           0xBF808274u

#define PS2_IOP_REG_SPU2_BASE            0xBF900000u

#define PS2_GS_VRAM_SIZE                 0x00400000u
#define PS2_SPU2_WORK_RAM_SIZE           0x00200000u
#define PS2_MEMORY_CARD_SIZE             0x00800000u

#endif /* KOSLOAD_PS2_MEMORY_MAP_H */
