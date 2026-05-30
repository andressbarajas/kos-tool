/* client/gamecube/exi.h */
/*
 * GameCube EXI (External Interface) bus driver.
 *
 * The EXI bus provides serial communication with external devices.
 * Three channels are available, each supporting multiple devices.
 * USBGecko is typically on Channel 1, Device 0.
 *
 * Channel 0 Status Register (0xCC006800):
 *   Bit 0:     EXIINTMASK  - EXI Interrupt Mask
 *   Bit 1:     EXIINT      - EXI Interrupt (write-1-to-clear)
 *   Bit 2:     TCINTMASK   - Transfer Complete Interrupt Mask
 *   Bit 3:     TCINT       - Transfer Complete Interrupt (write-1-to-clear)
 *   Bits 4-6:  CLK         - Clock frequency (0=1M,1=2M,2=4M,3=8M,4=16M,5=32M)
 *   Bit 7:     CS0B        - Chip Select Device 0
 *   Bit 8:     CS1B        - Chip Select Device 1
 *   Bit 9:     CS2B        - Chip Select Device 2
 *   Bit 10:    EXTINTMASK  - External Insert Interrupt Mask (ch0/ch1 only)
 *   Bit 11:    EXTINT      - External Insert Interrupt (W1C, ch0/ch1 only)
 *   Bit 12:    EXT         - External Device Present (read-only, ch0/ch1 only)
 *   Bit 13:    ROMDIS      - IPL ROM Disable (channel 0 only)
 */

#ifndef KOSLOAD_GC_EXI_H
#define KOSLOAD_GC_EXI_H

#include <stdint.h>

/* EXI register base addresses */
#define EXI_BASE            0xCC006800

#define EXI_CH0_BASE        (EXI_BASE + 0x00)
#define EXI_CH1_BASE        (EXI_BASE + 0x14)
#define EXI_CH2_BASE        (EXI_BASE + 0x28)

/* Register offsets within each channel */
#define EXI_STATUS          0x00    /* Channel Parameter Register */
#define EXI_DMA_ADDR        0x04    /* DMA Start Address */
#define EXI_DMA_LEN         0x08    /* DMA Transfer Length */
#define EXI_DMA_CR          0x0C    /* DMA Control Register */
#define EXI_DATA            0x10    /* Immediate Data Register */

/* EXI_STATUS bits */
#define EXI_STATUS_EXIINTMASK (1 << 0)
#define EXI_STATUS_EXIINT     (1 << 1) /* W1C */
#define EXI_STATUS_TCINTMASK  (1 << 2)
#define EXI_STATUS_TCINT      (1 << 3) /* W1C */
#define EXI_STATUS_CLK_MASK   (7 << 4)
#define EXI_STATUS_CS0B       (1 << 7)
#define EXI_STATUS_CS1B       (1 << 8)
#define EXI_STATUS_CS2B       (1 << 9)
#define EXI_STATUS_EXTINTMASK (1 << 10) /* ch0/ch1 only */
#define EXI_STATUS_EXTINT     (1 << 11) /* W1C, ch0/ch1 only */
#define EXI_STATUS_EXT        (1 << 12) /* read-only, ch0/ch1 only */
#define EXI_STATUS_ROMDIS     (1 << 13) /* channel 0 only */

/* Mask of all W1C bits (write-1-to-clear) */
#define EXI_STATUS_W1C_MASK (EXI_STATUS_EXIINT | EXI_STATUS_TCINT | EXI_STATUS_EXTINT)

/* Clock speeds (value for CLK field, pre-shifted to bits 4-6) */
#define EXI_CLK_1MHZ        (0 << 4)
#define EXI_CLK_2MHZ        (1 << 4)
#define EXI_CLK_4MHZ        (2 << 4)
#define EXI_CLK_8MHZ        (3 << 4)
#define EXI_CLK_16MHZ       (4 << 4)
#define EXI_CLK_32MHZ       (5 << 4)

/* Device select bits in STATUS register */
#define EXI_DEV0             (1 << 7)
#define EXI_DEV1             (1 << 8)
#define EXI_DEV2             (1 << 9)

/* DMA control bits */
#define EXI_DMA_START       (1 << 0)
#define EXI_DMA_READ        (0 << 2)
#define EXI_DMA_WRITE       (1 << 2)
#define EXI_DMA_READWRITE   (2 << 2)
#define EXI_IMM_READ        (0 << 2)
#define EXI_IMM_WRITE       (1 << 2)
#define EXI_IMM_READWRITE   (2 << 2)

/* Initialize the EXI subsystem */
void exi_init(void);

/* Select a device on a channel */
void exi_select(int channel, int device, int clock);

/* Deselect the current device on a channel */
void exi_deselect(int channel);

/* Immediate mode read/write (up to 4 bytes) */
uint32_t exi_imm(int channel, uint32_t data, int len, int mode);

/* DMA transfer (address must be 32-byte aligned) */
void exi_dma(int channel, void *addr, int len, int mode);

/* Read device ID */
uint32_t exi_get_id(int channel, int device);

/* Read channel status register (EXT bit, etc.) */
uint32_t exi_get_status(int channel);

#endif /* KOSLOAD_GC_EXI_H */
