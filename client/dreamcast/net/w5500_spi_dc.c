/* client/dreamcast/net/w5500_spi_dc.c */
/*
 * Dreamcast SCIF bit-bang SPI backend for W5500 driver.
 *
 * Uses the SH4 SCIF (Serial Communication Interface with FIFO) port
 * pins in direct I/O mode to implement SPI Mode 0 (CPOL=0, CPHA=0):
 *
 *   SCIF Pin    SPI Function    SCSPTR2 Bit
 *   --------    ------------    -----------
 *   TxD         MOSI            SPB2DT (bit 0), SPB2IO (bit 1)
 *   RxD         MISO            Read via SPB2DT after clock edge
 *   CTS         SCLK            CTSDT (bit 4), CTSIO (bit 5)
 *   RTS         CS#             RTSDT (bit 6), RTSIO (bit 7)
 *
 * Based on scif-spi.c from KallistiOS by SWAT (Ruslan Rostovtsev):
 *   https://github.com/KallistiOS/KallistiOS/pull/1261
 * The bit-bang runs at roughly 1 MHz on a stock 200 MHz SH4.
 *
 * IMPORTANT: This driver conflicts with the serial transport (dcload-serial).
 * It should only be compiled into the IP (network) firmware build.
 */

#include "../serial/scif.h"
#include "../../common/drivers/w5500.h"

/* ===== SCIF Port Register Direct Access ===== */

/*
 * We take over SCSPTR2 (Serial Port Register) for direct pin control.
 * Normal SCIF FIFO mode is disabled; we bit-bang everything.
 */
#define SCSPTR2_REG  (*(volatile unsigned short *)0xffe80020)
#define SCSCR2_REG   (*(volatile unsigned short *)0xffe80008)
#define SCSMR2_REG   (*(volatile unsigned short *)0xffe80000)
#define SCFCR2_REG   (*(volatile unsigned short *)0xffe80018)
#define SCFSR2_REG   (*(volatile unsigned short *)0xffe80010)
#define SCLSR2_REG   (*(volatile unsigned short *)0xffe80024)

/* SCSPTR2 bit definitions */
#define PTR2_RTSIO   (1 << 7) /* RTS pin direction: 1=output */
#define PTR2_RTSDT   (1 << 6) /* RTS pin data (active low CS) */
#define PTR2_CTSIO   (1 << 5) /* CTS pin direction: 1=output */
#define PTR2_CTSDT   (1 << 4) /* CTS pin data (SCLK) */
#define PTR2_SPB2IO  (1 << 1) /* TxD pin direction: 1=output */
#define PTR2_SPB2DT  (1 << 0) /* TxD/RxD pin data (MOSI/MISO) */

static unsigned short scsptr2_base;
static int spi_initialized;

/* ===== SPI Implementation ===== */

static int dc_w5500_spi_init(void) {
    if(spi_initialized)
        return -1;

    /* Disable SCIF transmit/receive, clear FIFO */
    SCSCR2_REG = 0;
    SCFCR2_REG = 0x06; /* Reset TX and RX FIFOs */
    SCFCR2_REG = 0;
    SCSMR2_REG = 0;
    SCFSR2_REG = 0;
    SCLSR2_REG = 0;

    /* Configure pin directions:
     *   RTS = output (CS#, active low — start deasserted = high)
     *   CTS = output (SCLK — start low for SPI mode 0)
     *   TxD = output (MOSI)
     *   RxD = input  (MISO — SPB2IO=0 would be input, but we read via SPB2DT)
     */
    scsptr2_base = PTR2_RTSIO | PTR2_RTSDT | PTR2_CTSIO | PTR2_SPB2IO;
    SCSPTR2_REG = scsptr2_base;

    spi_initialized = 1;
    return 0;
}

static void dc_w5500_spi_shutdown(void) {
    if(!spi_initialized)
        return;

    spi_initialized = 0;

    /* Restore SCIF to normal serial mode so dcload-serial can work
     * if we ever switch back. Re-init at the default baud. */
    scif_init(57600);
}

static void dc_w5500_spi_cs_assert(void) {
    /* CS# active low: clear RTSDT */
    scsptr2_base &= ~PTR2_RTSDT;
    SCSPTR2_REG = scsptr2_base;
}

static void dc_w5500_spi_cs_deassert(void) {
    /* CS# inactive: set RTSDT */
    scsptr2_base |= PTR2_RTSDT;
    SCSPTR2_REG = scsptr2_base;
}

/*
 * Write one byte out MOSI, MSB first.
 * SPI Mode 0: data setup on falling SCLK, sampled on rising SCLK.
 * We set the data bit, then pulse SCLK high, then low.
 */
static void dc_w5500_spi_write_byte(unsigned char val) {
    unsigned short tmp = scsptr2_base & ~PTR2_CTSDT & ~PTR2_SPB2DT;
    unsigned char bit;
    int i;

    for(i = 7; i >= 0; i--) {
        bit = (val >> i) & 0x01;
        SCSPTR2_REG = tmp | bit;              /* Set MOSI, SCLK low */
        SCSPTR2_REG = tmp | bit | PTR2_CTSDT; /* SCLK high (W5500 samples) */
    }
    /* Leave SCLK low */
    SCSPTR2_REG = tmp;
}

/*
 * Read one byte from MISO, MSB first.
 * Drive MOSI high (0xFF idle) while clocking.
 */
static unsigned char dc_w5500_spi_read_byte(void) {
    unsigned short tmp = (scsptr2_base & ~PTR2_CTSDT) | PTR2_SPB2DT;
    unsigned char val = 0;
    int i;

    for(i = 0; i < 8; i++) {
        SCSPTR2_REG = tmp;                              /* SCLK low, MOSI high */
        SCSPTR2_REG = tmp | PTR2_CTSDT;                 /* SCLK high */
        val = (val << 1) | (SCSPTR2_REG & PTR2_SPB2DT); /* Sample MISO */
    }

    return val;
}

/*
 * Write multiple bytes (MOSI).
 * Just loops write_byte for simplicity. Could be optimized with
 * unrolled bit-bang like KOS scif-spi.c for higher throughput.
 */
static void dc_w5500_spi_write_data(const unsigned char *buf, int len) {
    int i;
    for(i = 0; i < len; i++)
        dc_w5500_spi_write_byte(buf[i]);
}

/*
 * Read multiple bytes (MISO).
 */
static void dc_w5500_spi_read_data(unsigned char *buf, int len) {
    int i;
    for(i = 0; i < len; i++)
        buf[i] = dc_w5500_spi_read_byte();
}

/* ===== SPI Ops Table ===== */

const w5500_spi_ops_t dc_w5500_spi_ops = {
    dc_w5500_spi_init,
    dc_w5500_spi_shutdown,
    dc_w5500_spi_cs_assert,
    dc_w5500_spi_cs_deassert,
    dc_w5500_spi_write_byte,
    dc_w5500_spi_read_byte,
    dc_w5500_spi_write_data,
    dc_w5500_spi_read_data,
};
