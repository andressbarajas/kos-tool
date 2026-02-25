/* client/gamecube/exi.c */
/*
 * GameCube EXI (External Interface) bus driver.
 *
 * Provides low-level access to the EXI bus for communicating with
 * external devices (memory cards, USBGecko, BBA, etc).
 *
 * Important: Channel 0 has the ROMDIS bit (bit 13) which disables
 * the IPL ROM on device 1. We keep ROMDIS set after init to prevent
 * the IPL ROM from interfering with device 0 operations.
 */

#include "exi.h"

/* Volatile register access helpers */
static volatile uint32_t *exi_reg(int channel, int offset)
{
    uint32_t base = EXI_BASE + (channel * 0x14);
    return (volatile uint32_t *)(base + offset);
}

void exi_init(void)
{
    int ch;

    for (ch = 0; ch < 3; ch++) {
        /* Stop any in-progress DMA transfer */
        *exi_reg(ch, EXI_DMA_CR) = 0;

        /* Clear all W1C interrupt bits, deselect all devices,
         * disable interrupt masks.
         *
         * For channel 0: also set ROMDIS to disable the IPL ROM.
         * The IPL ROM on ch0 device 1 can cause bus contention
         * when enabled and we're accessing device 0 (memory card slot).
         *
         * Write-1-to-clear bits: EXIINT(1), TCINT(3), EXTINT(11).
         * Writing 1 to these clears them; writing 0 has no effect. */
        if (ch == 0)
            *exi_reg(ch, EXI_STATUS) = EXI_STATUS_W1C_MASK | EXI_STATUS_ROMDIS;
        else
            *exi_reg(ch, EXI_STATUS) = EXI_STATUS_W1C_MASK;
    }
}

void exi_select(int channel, int device, int clock)
{
    volatile uint32_t *status = exi_reg(channel, EXI_STATUS);
    uint32_t val;

    /* Set device chip select and clock.
     * For channel 0, preserve ROMDIS to keep IPL ROM disabled. */
    val = (1 << (7 + device)) | (clock & EXI_STATUS_CLK_MASK);
    if (channel == 0)
        val |= EXI_STATUS_ROMDIS;

    *status = val;
}

void exi_deselect(int channel)
{
    /* Deselect all devices (clear CS bits).
     * For channel 0, preserve ROMDIS. */
    if (channel == 0)
        *exi_reg(channel, EXI_STATUS) = EXI_STATUS_ROMDIS;
    else
        *exi_reg(channel, EXI_STATUS) = 0;
}

uint32_t exi_imm(int channel, uint32_t data, int len, int mode)
{
    volatile uint32_t *cr = exi_reg(channel, EXI_DMA_CR);
    volatile uint32_t *imm = exi_reg(channel, EXI_DATA);

    /* Write data to immediate register */
    *imm = data;

    /* Start immediate transfer: length in bytes (bits 4-5), mode (bits 2-3).
     * Mode constants (EXI_IMM_READ/WRITE/READWRITE) are pre-shifted to bits 2-3. */
    *cr = ((len - 1) << 4) | mode | EXI_DMA_START;

    /* Wait for transfer complete */
    while (*cr & EXI_DMA_START)
        ;

    /* Return data from immediate register */
    return *imm;
}

void exi_dma(int channel, void *addr, int len, int mode)
{
    volatile uint32_t *dma_addr = exi_reg(channel, EXI_DMA_ADDR);
    volatile uint32_t *dma_len = exi_reg(channel, EXI_DMA_LEN);
    volatile uint32_t *cr = exi_reg(channel, EXI_DMA_CR);

    /* Set DMA address (physical, 32-byte aligned) */
    *dma_addr = (uint32_t)addr & 0x1FFFFFFF;

    /* Set DMA length */
    *dma_len = len;

    /* Start DMA: bit 0 = start, bit 1 = DMA mode, bits 2-3 = direction.
     * Mode constants (EXI_DMA_READ/WRITE/READWRITE) are pre-shifted to bits 2-3. */
    *cr = mode | 0x03;

    /* Wait for DMA complete */
    while (*cr & EXI_DMA_START)
        ;
}

uint32_t exi_get_id(int channel, int device)
{
    uint32_t id;

    exi_select(channel, device, EXI_CLK_1MHZ);

    /* Send ID command (0x00000000) and read 4 bytes back */
    exi_imm(channel, 0x00000000, 2, EXI_IMM_WRITE);
    id = exi_imm(channel, 0x00000000, 4, EXI_IMM_READ);

    exi_deselect(channel);

    return id;
}

uint32_t exi_get_status(int channel)
{
    return *exi_reg(channel, EXI_STATUS);
}
