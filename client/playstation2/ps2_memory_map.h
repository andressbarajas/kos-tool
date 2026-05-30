/* client/playstation2/ps2_memory_map.h
 *
 * Common PlayStation 2 EE/IOP memory-map constants.
 *
 * This header contains addresses and simple address-conversion helpers only.
 * It is intentionally usable from both EE and cleanroom IOP translation units:
 * no libc headers, no inline code, and no SDK-owned names.
 */

#ifndef KOSLOAD_PS2_MEMORY_MAP_H
#define KOSLOAD_PS2_MEMORY_MAP_H

/* ------------------------------------------------------------------------- */
/* EE virtual segments and RAM aliases.                                      */
/* ------------------------------------------------------------------------- */

#define PS2_EE_KUSEG_BASE                0x00000000
#define PS2_EE_KUSEG_END                 0x7FFFFFFF
#define PS2_EE_KSEG0_BASE                0x80000000
#define PS2_EE_KSEG0_END                 0x9FFFFFFF
#define PS2_EE_KSEG1_BASE                0xA0000000
#define PS2_EE_KSEG1_END                 0xBFFFFFFF
#define PS2_EE_KSSEG_BASE                0xC0000000
#define PS2_EE_KSSEG_END                 0xDFFFFFFF
#define PS2_EE_KSEG3_BASE                0xE0000000
#define PS2_EE_KSEG3_END                 0xFFFFFFFF

#define PS2_EE_MAIN_RAM_PHYS_BASE        0x00000000
#define PS2_EE_MAIN_RAM_SIZE             0x02000000
#define PS2_EE_KERNEL_RESERVED_SIZE      0x00100000
#define PS2_EE_MAIN_RAM_CACHED_BASE      0x00000000
#define PS2_EE_MAIN_RAM_UNCACHED_BASE    0x20000000
#define PS2_EE_MAIN_RAM_ACCEL_BASE       0x30100000
#define PS2_EE_MAIN_RAM_ACCEL_PHYS_BASE  0x00100000
#define PS2_EE_MAIN_RAM_ACCEL_SIZE       0x01F00000

#define PS2_EE_SCRATCHPAD_BASE           0x70000000
#define PS2_EE_SCRATCHPAD_SIZE           0x00004000

#define PS2_EE_BIOS_PHYS_BASE            0x1FC00000
#define PS2_EE_BIOS_SIZE                 0x00400000
#define PS2_EE_BIOS_CACHED_BASE          0x9FC00000
#define PS2_EE_BIOS_UNCACHED_BASE        0xBFC00000

#define PS2_EE_IOP_RAM_BASE              0x1C000000
#define PS2_EE_IOP_RAM_UNCACHED_BASE     0xBC000000

#define PS2_EE_PHYS(addr) \
    ((unsigned int)(addr) & 0x1FFFFFFF)
#define PS2_EE_KSEG0_ADDR(addr) \
    (PS2_EE_KSEG0_BASE | PS2_EE_PHYS(addr))
#define PS2_EE_KSEG1_ADDR(addr) \
    (PS2_EE_KSEG1_BASE | PS2_EE_PHYS(addr))

/* ------------------------------------------------------------------------- */
/* EE hardware registers.  These are KSEG1, uncached addresses for C code.    */
/* ------------------------------------------------------------------------- */

#define PS2_EE_REG_IO_PHYS_BASE          0x10000000
#define PS2_EE_REG_IO_BASE               0xB0000000

#define PS2_EE_REG_TIMER0_BASE           0xB0000000
#define PS2_EE_REG_TIMER1_BASE           0xB0000800
#define PS2_EE_REG_TIMER2_BASE           0xB0001000
#define PS2_EE_REG_TIMER3_BASE           0xB0001800
#define PS2_EE_TIMER_COUNT_OFFSET        0x00
#define PS2_EE_TIMER_MODE_OFFSET         0x10
#define PS2_EE_TIMER_COMP_OFFSET         0x20
#define PS2_EE_TIMER_HOLD_OFFSET         0x30

#define PS2_EE_REG_IPU_CMD               0xB0002000
#define PS2_EE_REG_IPU_CTRL              0xB0002010
#define PS2_EE_REG_IPU_BP                0xB0002020
#define PS2_EE_REG_IPU_TOP               0xB0002030
#define PS2_EE_REG_IPU_OUT_FIFO          0xB0007000
#define PS2_EE_REG_IPU_IN_FIFO           0xB0007010

#define PS2_EE_REG_GIF_CTRL              0xB0003000
#define PS2_EE_REG_GIF_MODE              0xB0003010
#define PS2_EE_REG_GIF_STAT              0xB0003020
#define PS2_EE_REG_GIF_TAG0              0xB0003040
#define PS2_EE_REG_GIF_TAG1              0xB0003050
#define PS2_EE_REG_GIF_TAG2              0xB0003060
#define PS2_EE_REG_GIF_TAG3              0xB0003070
#define PS2_EE_REG_GIF_CNT               0xB0003080
#define PS2_EE_REG_GIF_P3CNT             0xB0003090
#define PS2_EE_REG_GIF_P3TAG             0xB00030A0
#define PS2_EE_REG_GIF_FIFO              0xB0006000

#define PS2_EE_REG_DMAC_VIF0_BASE        0xB0008000
#define PS2_EE_REG_DMAC_VIF1_BASE        0xB0009000
#define PS2_EE_REG_DMAC_GIF_BASE         0xB000A000
#define PS2_EE_REG_DMAC_IPU_FROM_BASE    0xB000B000
#define PS2_EE_REG_DMAC_IPU_TO_BASE      0xB000B400
#define PS2_EE_REG_DMAC_SIF0_BASE        0xB000C000
#define PS2_EE_REG_DMAC_SIF1_BASE        0xB000C400
#define PS2_EE_REG_DMAC_SIF2_BASE        0xB000C800
#define PS2_EE_REG_DMAC_SPR_FROM_BASE    0xB000D000
#define PS2_EE_REG_DMAC_SPR_TO_BASE      0xB000D400

#define PS2_EE_DMAC_CHCR_OFFSET          0x00
#define PS2_EE_DMAC_MADR_OFFSET          0x10
#define PS2_EE_DMAC_QWC_OFFSET           0x20
#define PS2_EE_DMAC_TADR_OFFSET          0x30
#define PS2_EE_DMAC_ASR0_OFFSET          0x40
#define PS2_EE_DMAC_ASR1_OFFSET          0x50
#define PS2_EE_DMAC_SADR_OFFSET          0x80

#define PS2_EE_REG_DMAC_SIF0_CHCR        0xB000C000
#define PS2_EE_REG_DMAC_SIF0_QWC         0xB000C020
#define PS2_EE_REG_DMAC_SIF1_CHCR        0xB000C400
#define PS2_EE_REG_DMAC_SIF1_QWC         0xB000C420
#define PS2_EE_REG_DMAC_SIF1_TADR        0xB000C430

#define PS2_EE_REG_DMAC_CTRL             0xB000E000
#define PS2_EE_REG_DMAC_STAT             0xB000E010
#define PS2_EE_REG_DMAC_PCR              0xB000E020
#define PS2_EE_REG_DMAC_SQWC             0xB000E030
#define PS2_EE_REG_DMAC_RBSR             0xB000E040
#define PS2_EE_REG_DMAC_RBOR             0xB000E050
#define PS2_EE_REG_DMAC_STADR            0xB000E060
#define PS2_EE_REG_DMAC_ENABLER          0xB000F520
#define PS2_EE_REG_DMAC_ENABLEW          0xB000F590

#define PS2_EE_REG_INTC_STAT             0xB000F000
#define PS2_EE_REG_INTC_MASK             0xB000F010

#define PS2_EE_REG_SIF_MSCOM             0xB000F200
#define PS2_EE_REG_SIF_SMCOM             0xB000F210
#define PS2_EE_REG_SIF_MSFLAG            0xB000F220
#define PS2_EE_REG_SIF_SMFLAG            0xB000F230
#define PS2_EE_REG_SIF_CTRL              0xB000F240
#define PS2_EE_REG_SIF_BD6               0xB000F260

#define PS2_EE_REG_KPUTCHAR              0xB000F180
#define PS2_EE_REG_MCH_DRD               0xB000F430
#define PS2_EE_REG_MCH_RICM              0xB000F440

#define PS2_EE_REG_GS_PRIV_PHYS_BASE     0x12000000
#define PS2_EE_REG_GS_PRIV_BASE          0xB2000000
#define PS2_EE_REG_GS_PMODE              0xB2000000
#define PS2_EE_REG_GS_SMODE1             0xB2000010
#define PS2_EE_REG_GS_SMODE2             0xB2000020
#define PS2_EE_REG_GS_SRFSH              0xB2000030
#define PS2_EE_REG_GS_SYNCH1             0xB2000040
#define PS2_EE_REG_GS_SYNCH2             0xB2000050
#define PS2_EE_REG_GS_SYNCV              0xB2000060
#define PS2_EE_REG_GS_DISPFB1            0xB2000070
#define PS2_EE_REG_GS_DISPLAY1           0xB2000080
#define PS2_EE_REG_GS_DISPFB2            0xB2000090
#define PS2_EE_REG_GS_DISPLAY2           0xB20000A0
#define PS2_EE_REG_GS_EXTBUF             0xB20000B0
#define PS2_EE_REG_GS_EXTDATA            0xB20000C0
#define PS2_EE_REG_GS_EXTWRITE           0xB20000D0
#define PS2_EE_REG_GS_BGCOLOR            0xB20000E0
#define PS2_EE_REG_GS_CSR                0xB2001000
#define PS2_EE_REG_GS_IMR                0xB2001010
#define PS2_EE_REG_GS_BUSDIR             0xB2001040
#define PS2_EE_REG_GS_SIGLBLID           0xB2001080

/* ------------------------------------------------------------------------- */
/* IOP virtual segments, memory, and hardware-register aliases.              */
/* ------------------------------------------------------------------------- */

#define PS2_IOP_KUSEG_BASE               0x00000000
#define PS2_IOP_KUSEG_END                0x7FFFFFFF
#define PS2_IOP_KSEG0_BASE               0x80000000
#define PS2_IOP_KSEG0_END                0x9FFFFFFF
#define PS2_IOP_KSEG1_BASE               0xA0000000
#define PS2_IOP_KSEG1_END                0xBFFFFFFF

#define PS2_IOP_MAIN_RAM_PHYS_BASE       0x00000000
#define PS2_IOP_MAIN_RAM_SIZE            0x00200000
#define PS2_IOP_BIOS_PHYS_BASE           0x1FC00000
#define PS2_IOP_BIOS_SIZE                0x00400000
#define PS2_IOP_BIOS_CACHED_BASE         0x9FC00000
#define PS2_IOP_BIOS_UNCACHED_BASE       0xBFC00000

#define PS2_IOP_REG_SIF_PHYS_BASE        0x1D000000
#define PS2_IOP_REG_IO_PHYS_BASE         0x1F800000
#define PS2_IOP_REG_IO_SIZE              0x00010000
#define PS2_IOP_REG_SPU2_PHYS_BASE       0x1F900000
#define PS2_IOP_REG_CACHE_CTRL_BASE      0xFFFE0000

#define PS2_IOP_PHYS(addr) \
    ((unsigned int)(addr) & 0x1FFFFFFF)
#define PS2_IOP_KSEG0_ADDR(addr) \
    (PS2_IOP_KSEG0_BASE | PS2_IOP_PHYS(addr))
#define PS2_IOP_KSEG1_ADDR(addr) \
    (PS2_IOP_KSEG1_BASE | PS2_IOP_PHYS(addr))

#define PS2_IOP_REG_SIF_BASE             0xBD000000
#define PS2_IOP_REG_SIF_MSCOM            0xBD000000
#define PS2_IOP_REG_SIF_SMCOM            0xBD000010
#define PS2_IOP_REG_SIF_MSFLAG           0xBD000020
#define PS2_IOP_REG_SIF_SMFLAG           0xBD000030
#define PS2_IOP_REG_SIF_CTRL             0xBD000040
#define PS2_IOP_REG_SIF_BD6              0xBD000060

#define PS2_IOP_REG_CDVD_BASE            0xBF402000
#define PS2_IOP_REG_CDVD_NCMD            0xBF402004
#define PS2_IOP_REG_CDVD_NCMD_STAT       0xBF402005
#define PS2_IOP_REG_CDVD_ERROR           0xBF402006
#define PS2_IOP_REG_CDVD_BREAK           0xBF402007
#define PS2_IOP_REG_CDVD_ISTAT           0xBF402008
#define PS2_IOP_REG_CDVD_STATUS          0xBF40200A
#define PS2_IOP_REG_CDVD_DISK_TYPE       0xBF40200F
#define PS2_IOP_REG_CDVD_SCMD            0xBF402016
#define PS2_IOP_REG_CDVD_SCMD_STAT       0xBF402017
#define PS2_IOP_REG_CDVD_SCMD_PARAMS     0xBF402018

#define PS2_IOP_REG_INTC_STAT            0xBF801070
#define PS2_IOP_REG_INTC_MASK            0xBF801074
#define PS2_IOP_REG_INTC_CTRL            0xBF801078

#define PS2_IOP_REG_DMA_OLD_CHANNEL_BASE 0xBF801080
#define PS2_IOP_REG_DMA_NEW_CHANNEL_BASE 0xBF801500
#define PS2_IOP_REG_DMA_DPCR             0xBF8010F0
#define PS2_IOP_REG_DMA_DICR             0xBF8010F4
#define PS2_IOP_REG_DMA_DPCR2            0xBF801570
#define PS2_IOP_REG_DMA_DICR2            0xBF801574
#define PS2_IOP_REG_DMA_DMACEN           0xBF801578
#define PS2_IOP_REG_DMA_DMACINTEN        0xBF80157C
#define PS2_IOP_REG_DMA_DPCR3            0xBF8015F0

#define PS2_IOP_REG_TIMER0_BASE          0xBF801100
#define PS2_IOP_REG_TIMER1_BASE          0xBF801110
#define PS2_IOP_REG_TIMER2_BASE          0xBF801120
#define PS2_IOP_REG_TIMER3_BASE          0xBF801480
#define PS2_IOP_REG_TIMER4_BASE          0xBF801490
#define PS2_IOP_REG_TIMER5_BASE          0xBF8014A0

#define PS2_IOP_REG_SIO2_BASE            0xBF808200
#define PS2_IOP_REG_SIO2_SEND3_BASE      0xBF808200
#define PS2_IOP_REG_SIO2_SEND12_BASE     0xBF808240
#define PS2_IOP_REG_SIO2_IN_FIFO         0xBF808260
#define PS2_IOP_REG_SIO2_OUT_FIFO        0xBF808264
#define PS2_IOP_REG_SIO2_CTRL            0xBF808268
#define PS2_IOP_REG_SIO2_RECV1           0xBF80826C
#define PS2_IOP_REG_SIO2_RECV2           0xBF808270
#define PS2_IOP_REG_SIO2_RECV3           0xBF808274

#define PS2_IOP_REG_SPU2_BASE            0xBF900000

#define PS2_GS_VRAM_SIZE                 0x00400000
#define PS2_SPU2_WORK_RAM_SIZE           0x00200000
#define PS2_MEMORY_CARD_SIZE             0x00800000

#endif /* KOSLOAD_PS2_MEMORY_MAP_H */
