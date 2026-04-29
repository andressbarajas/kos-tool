/* client/gamecube/net/w5500_spi_gc.c */
/*
 * GameCube EXI SPI backend for W5500 driver.
 *
 * The W5500 is accessed via SPI-over-EXI, similar to the ENC28J60.
 * Possible EXI locations (same as ENC28J60):
 *   - Memory Card Slot A:  Channel 0, Device 0  (32 MHz)
 *   - Memory Card Slot B:  Channel 1, Device 0  (32 MHz)
 *   - Serial Port 1 (BBA): Channel 0, Device 2  (16 MHz max)
 *   - Serial Port 2:       Channel 2, Device 0  (32 MHz)
 *
 * Detection probes all locations, soft-resets the W5500, and checks
 * the version register (0x04) to confirm a W5500 is present.
 */

#include "../../common/drivers/w5500.h"
#include "../exi.h"

/* Runtime EXI location (set by detection) */
static int w5500_channel = -1;
static int w5500_device = -1;

/* ===== EXI helpers ===== */

static int w5500_exi_speed(void)
{
    /* Serial Port 1 (ch0, dev2) is limited to 16 MHz */
    if (w5500_channel == 0 && w5500_device == 2)
        return EXI_CLK_16MHZ;
    return EXI_CLK_32MHZ;
}

/* ===== SPI Implementation via EXI ===== */

static int gc_w5500_spi_init(void)
{
    /* EXI is already initialized by exi_init() in the GC startup code.
     * Nothing additional needed here. */
    return 0;
}

static void gc_w5500_spi_shutdown(void)
{
    /* Nothing to do — EXI stays initialized */
}

static void gc_w5500_spi_cs_assert(void)
{
    exi_select(w5500_channel, w5500_device, w5500_exi_speed());
}

static void gc_w5500_spi_cs_deassert(void)
{
    exi_deselect(w5500_channel);
}

/*
 * Write one byte via EXI immediate mode.
 * EXI data register is left-aligned: byte goes into bits [31:24].
 */
static void gc_w5500_spi_write_byte(unsigned char val)
{
    exi_imm(w5500_channel, (unsigned int)val << 24, 1, EXI_IMM_WRITE);
}

/*
 * Read one byte via EXI immediate mode.
 * EXI returns data left-aligned: byte is in bits [31:24].
 */
static unsigned char gc_w5500_spi_read_byte(void)
{
    unsigned int val = exi_imm(w5500_channel, 0, 1, EXI_IMM_READ);
    return (val >> 24) & 0xFF;
}

/*
 * Write multiple bytes via EXI.
 * For simplicity, we use byte-by-byte immediate transfers.
 * Could be optimized with DMA for large transfers (>= 32 bytes).
 */
static void gc_w5500_spi_write_data(const unsigned char *buf, int len)
{
    int i;
    for (i = 0; i < len; i++)
        exi_imm(w5500_channel, (unsigned int)buf[i] << 24, 1, EXI_IMM_WRITE);
}

/*
 * Read multiple bytes via EXI.
 * Byte-by-byte for simplicity; DMA could be used for aligned buffers.
 */
static void gc_w5500_spi_read_data(unsigned char *buf, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        unsigned int val = exi_imm(w5500_channel, 0, 1, EXI_IMM_READ);
        buf[i] = (val >> 24) & 0xFF;
    }
}

/* ===== SPI Ops Table ===== */

const w5500_spi_ops_t gc_w5500_spi_ops = {
    gc_w5500_spi_init,
    gc_w5500_spi_shutdown,
    gc_w5500_spi_cs_assert,
    gc_w5500_spi_cs_deassert,
    gc_w5500_spi_write_byte,
    gc_w5500_spi_read_byte,
    gc_w5500_spi_write_data,
    gc_w5500_spi_read_data,
};

/* ===== EXI-level W5500 Detection =====
 *
 * The W5500 doesn't have a standard EXI device ID like the BBA or ENC28J60.
 * Instead we probe each location by:
 *   1. Select device at 1 MHz (safe for unknown devices)
 *   2. Send a W5500 soft-reset command
 *   3. Wait for reset to complete
 *   4. Read the version register (should return 0x04)
 *
 * This is non-destructive for non-W5500 devices (they'll ignore
 * the SPI commands or return wrong version).
 */

static int w5500_probe_exi(int channel, int device)
{
    unsigned char cmd[4];
    unsigned char ver;

    /* EXT bit guard: Skip empty memory card slots to avoid sending 
     * W5500 reset/read commands to unrelated devices. */
    if (device == 0 && channel < 2) {
        if (!(exi_get_status(channel) & EXI_STATUS_EXT))
            return 0;
    }

    /* Select at 1 MHz for safe probing */
    exi_select(channel, device, EXI_CLK_1MHZ);

    /* Send W5500 soft-reset: write 0x80 to MR (addr 0x0000, common block) */
    cmd[0] = 0x00;     /* Address high byte */
    cmd[1] = 0x00;     /* Address low byte */
    cmd[2] = (W5500_COMMON_BLOCK << 3) | W5500_SPI_WRITE | W5500_SPI_VDM;
    cmd[3] = W5500_MR_RST;

    exi_imm(channel, ((unsigned int)cmd[0] << 24) | ((unsigned int)cmd[1] << 16) |
                      ((unsigned int)cmd[2] << 8) | cmd[3], 4, EXI_IMM_WRITE);

    exi_deselect(channel);

    /* Wait for reset — ~1ms */
    {
        volatile int i;
        for (i = 0; i < 100000; i++)
            ;
    }

    /* Read version register (addr 0x0039, common block) */
    exi_select(channel, device, EXI_CLK_1MHZ);

    /* Send read command: addr 0x0039, common block, read mode */
    cmd[0] = 0x00;     /* Address high byte (0x0039 >> 8) */
    cmd[1] = 0x39;     /* Address low byte */
    cmd[2] = (W5500_COMMON_BLOCK << 3) | W5500_SPI_READ | W5500_SPI_VDM;

    exi_imm(channel, ((unsigned int)cmd[0] << 24) | ((unsigned int)cmd[1] << 16) |
                      ((unsigned int)cmd[2] << 8), 3, EXI_IMM_WRITE);

    /* Read 1 byte response */
    ver = (unsigned char)((exi_imm(channel, 0, 1, EXI_IMM_READ) >> 24) & 0xFF);

    exi_deselect(channel);

    if (ver == W5500_VERSION_EXPECTED) {
        w5500_channel = channel;
        w5500_device = device;
        return 1;
    }

    return 0;
}

/*
 * Probe all EXI locations for a W5500.
 * Returns 1 if found, 0 if not.
 */
int w5500_probe_exi_all(void)
{
    /* Memory Card Slot A */
    if (w5500_probe_exi(0, 0))
        return 1;

    /* Memory Card Slot B */
    if (w5500_probe_exi(1, 0))
        return 1;

    /* Serial Port 1 (BBA slot) */
    if (w5500_probe_exi(0, 2))
        return 1;

    /* Serial Port 2 */
    if (w5500_probe_exi(2, 0))
        return 1;

    return 0;
}
