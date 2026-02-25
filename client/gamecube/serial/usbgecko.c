/* client/gamecube/serial/usbgecko.c */
/*
 * USBGecko serial driver for GameCube.
 *
 * Communicates with the USBGecko debug adapter via the EXI bus.
 * The USBGecko sits on EXI Channel 1, Device 0 and provides
 * bidirectional byte-level serial I/O over USB to the host PC.
 */

#include "usbgecko.h"
#include "exi.h"

/* Active USBGecko endpoint. Defaults to slot B (channel 1) for compatibility,
 * but usbgecko_detect() probes both slots and updates these at runtime. */
static int gecko_channel = USBGECKO_CHANNEL;
static int gecko_device = USBGECKO_DEVICE;

static int probe_gecko(int channel, int device)
{
    uint32_t val;
    uint16_t tx_status, rx_status;

    /* Quick check: EXT bit (read-only) indicates device present.
     * Available on channels 0 and 1 only. */
    if (!(exi_get_status(channel) & EXI_STATUS_EXT))
        return 0;

    /* Try standard EXI device ID first (fast path). */
    val = exi_get_id(channel, device);
    if (val == USBGECKO_ID)
        goto found;

    /* Many USB Gecko adapters don't respond to the standard EXI ID
     * command (returning 0x00000000 instead of 0x01010000). Fall back
     * to querying TX and RX status: a real USB Gecko returns status
     * words where only bit 10 (0x0400) may be set, while empty slots
     * return 0xFFFF from bus pull-ups and other EXI devices return
     * unpredictable values with other bits set. */
    exi_select(channel, device, EXI_CLK_32MHZ);
    val = exi_imm(channel, USBGECKO_CMD_TX_STATUS, 2, EXI_IMM_READWRITE);
    exi_deselect(channel);
    tx_status = (val >> 16) & 0xFFFF;

    exi_select(channel, device, EXI_CLK_32MHZ);
    val = exi_imm(channel, USBGECKO_CMD_RX_STATUS, 2, EXI_IMM_READWRITE);
    exi_deselect(channel);
    rx_status = (val >> 16) & 0xFFFF;

    /* Reject if any bits other than the status flag (0x0400) are set. */
    if ((tx_status & ~USBGECKO_TX_READY) != 0)
        return 0;
    if ((rx_status & ~USBGECKO_RX_READY) != 0)
        return 0;

found:
    gecko_channel = channel;
    gecko_device = device;
    return 1;
}

/* Check if TX FIFO has space */
static int check_tx_ready(void)
{
    uint32_t val;

    exi_select(gecko_channel, gecko_device, EXI_CLK_32MHZ);
    val = exi_imm(gecko_channel, USBGECKO_CMD_TX_STATUS, 2, EXI_IMM_READWRITE);
    exi_deselect(gecko_channel);

    return ((val >> 16) & USBGECKO_TX_READY) ? 1 : 0;
}

/* Check if RX FIFO has data */
static int check_rx_ready(void)
{
    uint32_t val;

    exi_select(gecko_channel, gecko_device, EXI_CLK_32MHZ);
    val = exi_imm(gecko_channel, USBGECKO_CMD_RX_STATUS, 2, EXI_IMM_READWRITE);
    exi_deselect(gecko_channel);

    return ((val >> 16) & USBGECKO_RX_READY) ? 1 : 0;
}

int usbgecko_detect(void)
{
    /* Prefer current/default location first, then fall back to both slots. */
    if (probe_gecko(gecko_channel, gecko_device))
        return 1;
    if (probe_gecko(1, USBGECKO_DEVICE))
        return 1;
    if (probe_gecko(0, USBGECKO_DEVICE))
        return 1;
    return 0;
}

int usbgecko_init(void)
{
    exi_init();

    if (!usbgecko_detect())
        return -1;

    /* Flush any stale data in the RX FIFO.
     * When booting from Swiss/SD card, leftover bytes from a previous
     * kos-tool session can sit in the USB FIFO. If a stale byte matches
     * a multi-byte command (e.g. 'B' for load), the command loop would
     * deadlock waiting for data that never comes. */
    while (check_rx_ready())
        usbgecko_getchar();

    return 0;
}

void usbgecko_putchar(unsigned char c)
{
    /* Wait until TX FIFO has space */
    while (!check_tx_ready())
        ;

    exi_select(gecko_channel, gecko_device, EXI_CLK_32MHZ);
    exi_imm(gecko_channel, USBGECKO_CMD_TX_DATA | ((uint32_t)c << 20), 2, EXI_IMM_READWRITE);
    exi_deselect(gecko_channel);
}

unsigned char usbgecko_getchar(void)
{
    uint32_t val;

    /* Wait until RX FIFO has data */
    while (!check_rx_ready())
        ;

    exi_select(gecko_channel, gecko_device, EXI_CLK_32MHZ);
    val = exi_imm(gecko_channel, USBGECKO_CMD_RX_DATA, 2, EXI_IMM_READWRITE);
    exi_deselect(gecko_channel);

    return (unsigned char)((val >> 16) & 0xFF);
}

int usbgecko_rx_ready(void)
{
    return check_rx_ready();
}

int usbgecko_tx_ready(void)
{
    return check_tx_ready();
}

void usbgecko_puts(const unsigned char *str)
{
    while (*str)
        usbgecko_putchar(*str++);
}

void usbgecko_flush(void)
{
    /* Wait for TX FIFO to become ready (all data sent) */
    while (!check_tx_ready())
        ;
}
