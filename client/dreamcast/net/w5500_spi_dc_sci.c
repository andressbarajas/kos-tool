/* client/dreamcast/net/w5500_spi_dc_sci.c */
/*
 * Dreamcast SCI hardware SPI backend for W5500 driver.
 *
 * Uses the SH4 SCI (Serial Communication Interface, channel 1) in
 * clocked synchronous mode as a hardware SPI master:
 *
 *   SCI Pin     SPI Function
 *   -------     ------------
 *   MD1/TxD1    MOSI
 *   MD2/RxD1    MISO
 *   MD0/SCK1    SCLK (internal clock, PCLK/4 = 12.5 MHz)
 *   PA7 GPIO    CS# (retail; NAOMI uses SCIF RTS instead)
 *
 * The SCI shifts LSB-first in synchronous mode while SPI is MSB-first,
 * so every byte is bit-reversed on its way in and out.
 *
 * Based on hardware/sci.c from KallistiOS by SWAT (Ruslan Rostovtsev),
 * stripped down to polled PIO for kos-tool's freestanding environment
 * (no threads, no malloc, no DMA).
 *
 * Unlike the SCIF bit-bang backend this leaves the SCIF port untouched,
 * but it is still only compiled into the IP (network) firmware build.
 */

#include "../../common/drivers/w5500.h"

/* ===== SCI Register Direct Access ===== */

#define SCSMR1_REG   (*(volatile unsigned char *)0xffe00000) /* Serial mode */
#define SCBRR1_REG   (*(volatile unsigned char *)0xffe00004) /* Bit rate */
#define SCSCR1_REG   (*(volatile unsigned char *)0xffe00008) /* Serial control */
#define SCTDR1_REG   (*(volatile unsigned char *)0xffe0000c) /* Transmit data */
#define SCSSR1_REG   (*(volatile unsigned char *)0xffe00010) /* Serial status */
#define SCRDR1_REG   (*(volatile unsigned char *)0xffe00014) /* Receive data */
#define SCSPTR1_REG  (*(volatile unsigned char *)0xffe00018) /* Serial port */

/* SCSMR1 bits */
#define SMR_CA       (1 << 7) /* Communication mode: 1 = clocked synchronous */

/* SCSCR1 bits */
#define SCR_TE       (1 << 5) /* Transmit enable */
#define SCR_RE       (1 << 4) /* Receive enable */

/* SCSSR1 bits */
#define SSR_TDRE     (1 << 7) /* Transmit data register empty */
#define SSR_RDRF     (1 << 6) /* Receive data register full */
#define SSR_ORER     (1 << 5) /* Overrun error */
#define SSR_FER      (1 << 4) /* Framing error */
#define SSR_PER      (1 << 3) /* Parity error */
#define SSR_TEND     (1 << 2) /* Transmit end */

/* Standby control — SCI module clock gate */
#define STBCR_REG    (*(volatile unsigned char *)0xffc00004)
#define STBCR_SCI_STP (1 << 0) /* 1 = SCI stopped, 0 = active */

/* GPIO port A — CS# on PA7 (retail Dreamcast) */
#define PCTRA_REG    (*(volatile unsigned int *)0xff80002c)
#define PDTRA_REG    (*(volatile unsigned short *)0xff800030)

#define CS_PIN_BIT   7
#define CS_PCTRA_MASK (3 << (CS_PIN_BIT * 2))
#define CS_PCTRA_OUT  (1 << (CS_PIN_BIT * 2))
#define CS_PDTRA_BIT  (1 << CS_PIN_BIT)

/* SCIF registers — CS# on RTS (NAOMI, no free GPIO on CN1) */
#define SCFCR2_REG   (*(volatile unsigned short *)0xffe80018)
#define SCSPTR2_REG  (*(volatile unsigned short *)0xffe80020)

#define SCFCR2_MCE   (1 << 3) /* Modem control enable */
#define PTR2_RTSIO   (1 << 7) /* RTS pin direction: 1=output */
#define PTR2_RTSDT   (1 << 6) /* RTS pin data (active low CS) */

/* System mode register — distinguishes retail DC from NAOMI */
#define SYSMODE_REG  (*(volatile unsigned int *)0xa05f74b0)
#define HW_TYPE_RETAIL 0x0

/* Bounded busy-waits so a wedged peripheral can't hang detection */
#define SCI_MAX_WAIT 500000

static int sci_initialized;
static int sci_cs_gpio;             /* 1 = PA7 GPIO CS, 0 = SCIF RTS CS */
static unsigned char sci_xfer_mode; /* Current TE/RE bits in SCSCR1 */

/*
 * SCI synchronous mode is LSB-first; SPI is MSB-first.
 */
static unsigned char bit_reverse8(unsigned char v) {
    v = (unsigned char)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
    v = (unsigned char)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = (unsigned char)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

static void sci_delay(volatile int count) {
    while(count-- > 0)
        ;
}

/*
 * Switch between transmit-only (TE) and full-duplex (TE|RE) with the
 * disable + settle + re-enable dance the SH4 requires between changes.
 */
static void sci_set_xfer_mode(unsigned char mode) {
    unsigned char reg;

    if(mode == 0) {
        SCSCR1_REG &= (unsigned char)~(SCR_TE | SCR_RE);
    }
    else if(sci_xfer_mode != mode) {
        reg = SCSCR1_REG;

        if(sci_xfer_mode) {
            reg &= (unsigned char)~(SCR_TE | SCR_RE);
            SCSCR1_REG = reg;
            sci_delay(200); /* ~1.5us settle */
        }
        reg |= mode;
        SCSCR1_REG = reg;
    }

    sci_xfer_mode = mode;
}

/* ===== SPI Implementation ===== */

static int dc_w5500_sci_spi_init(void) {
    unsigned int timeout = 0;

    if(sci_initialized)
        return -1;

    /* Wake the SCI module from standby */
    if(STBCR_REG & STBCR_SCI_STP) {
        STBCR_REG &= (unsigned char)~STBCR_SCI_STP;
        sci_delay(50000);
    }

    /* Disable transmit/receive, internal clock, SCK as clock output */
    SCSCR1_REG = 0;
    SCSPTR1_REG = 0;
    sci_xfer_mode = 0;

    /* Clocked synchronous mode, CKS=0; BRR=0 gives PCLK/4 = 12.5 Mbps */
    SCSMR1_REG = SMR_CA;
    SCBRR1_REG = 0;
    sci_delay(50000);

    /* Configure CS#: PA7 GPIO on retail, SCIF RTS on NAOMI */
    sci_cs_gpio = (((SYSMODE_REG >> 4) & 0x0f) == HW_TYPE_RETAIL);

    if(sci_cs_gpio) {
        /* PA7 as output, initial state inactive (high) */
        PCTRA_REG = (PCTRA_REG & ~CS_PCTRA_MASK) | CS_PCTRA_OUT;
        PDTRA_REG |= CS_PDTRA_BIT;
    }
    else {
        /* Take RTS away from modem control, drive it high (inactive) */
        SCFCR2_REG &= (unsigned short)~SCFCR2_MCE;
        SCSPTR2_REG |= (PTR2_RTSIO | PTR2_RTSDT);
    }

    /* Clear stale error flags and flush any pending receive byte */
    SCSSR1_REG &= (unsigned char)~(SSR_ORER | SSR_FER | SSR_PER);
    if(SCSSR1_REG & SSR_RDRF) {
        (void)SCRDR1_REG;
        SCSSR1_REG &= (unsigned char)~SSR_RDRF;
    }

    /* Wait for the transmitter to come up empty */
    while(!(SCSSR1_REG & SSR_TDRE)) {
        if(++timeout > SCI_MAX_WAIT)
            return -1;
    }

    sci_initialized = 1;
    return 0;
}

static void dc_w5500_sci_spi_shutdown(void) {
    if(!sci_initialized)
        return;

    sci_initialized = 0;

    /* Disable TX/RX and gate the module clock again */
    SCSCR1_REG = 0;
    sci_xfer_mode = 0;
    STBCR_REG |= STBCR_SCI_STP;

    /* Release the CS pin */
    if(sci_cs_gpio)
        PCTRA_REG &= ~CS_PCTRA_MASK;
    else
        SCSPTR2_REG &= (unsigned short)~(PTR2_RTSIO | PTR2_RTSDT);
}

static void dc_w5500_sci_spi_cs_assert(void) {
    if(sci_cs_gpio)
        PDTRA_REG &= (unsigned short)~CS_PDTRA_BIT;
    else
        SCSPTR2_REG &= (unsigned short)~PTR2_RTSDT;
}

static void dc_w5500_sci_spi_cs_deassert(void) {
    if(sci_cs_gpio)
        PDTRA_REG |= CS_PDTRA_BIT;
    else
        SCSPTR2_REG |= PTR2_RTSDT;
}

/*
 * Write one byte out MOSI. Transmit-only; waits for TEND so the last
 * clock edge is done before the caller may deassert CS.
 */
static void dc_w5500_sci_spi_write_byte(unsigned char val) {
    unsigned int timeout = 0;

    sci_set_xfer_mode(SCR_TE);

    while(!(SCSSR1_REG & SSR_TDRE)) {
        if(++timeout > SCI_MAX_WAIT)
            return;
    }

    SCTDR1_REG = bit_reverse8(val);
    SCSSR1_REG &= (unsigned char)~SSR_TDRE;

    timeout = 0;
    while(!(SCSSR1_REG & SSR_TEND)) {
        if(++timeout > SCI_MAX_WAIT)
            return;
    }
}

/*
 * Read one byte from MISO. The SCI only clocks while transmitting, so
 * run full-duplex and shift out a dummy 0xFF (MOSI idle high).
 */
static unsigned char dc_w5500_sci_spi_read_byte(void) {
    unsigned int timeout = 0;
    unsigned char status, val;

    sci_set_xfer_mode(SCR_TE | SCR_RE);

    while(!(SCSSR1_REG & SSR_TDRE)) {
        if(++timeout > SCI_MAX_WAIT)
            return 0xFF;
    }

    SCTDR1_REG = 0xFF;
    SCSSR1_REG &= (unsigned char)~SSR_TDRE;

    timeout = 0;
    do {
        status = SCSSR1_REG;

        if(status & SSR_ORER)
            SCSSR1_REG &= (unsigned char)~SSR_ORER;

        if(++timeout > SCI_MAX_WAIT)
            return 0xFF;
    } while(!(status & SSR_RDRF));

    val = SCRDR1_REG;
    SCSSR1_REG &= (unsigned char)~SSR_RDRF;

    return bit_reverse8(val);
}

/*
 * Write multiple bytes (MOSI). The TDR/TSR double buffer keeps the
 * clock running back-to-back; TEND is only awaited once at the end.
 */
static void dc_w5500_sci_spi_write_data(const unsigned char *buf, int len) {
    unsigned int timeout;
    int i;

    sci_set_xfer_mode(SCR_TE);

    for(i = 0; i < len; i++) {
        timeout = 0;
        while(!(SCSSR1_REG & SSR_TDRE)) {
            if(++timeout > SCI_MAX_WAIT)
                return;
        }

        SCTDR1_REG = bit_reverse8(buf[i]);
        SCSSR1_REG &= (unsigned char)~SSR_TDRE;
    }

    timeout = 0;
    while(!(SCSSR1_REG & SSR_TEND)) {
        if(++timeout > SCI_MAX_WAIT)
            return;
    }
}

/*
 * Read multiple bytes (MISO). TDR is preloaded with 0xFF once; each
 * TDRE clear re-sends it, keeping one byte in flight while the
 * previous one is pulled from RDR — no overrun is possible in this
 * lockstep, and the transmit side never stalls the clock.
 */
static void dc_w5500_sci_spi_read_data(unsigned char *buf, int len) {
    unsigned int timeout;
    unsigned char status;
    int i;

    sci_set_xfer_mode(SCR_TE | SCR_RE);

    timeout = 0;
    while(!(SCSSR1_REG & SSR_TDRE)) {
        if(++timeout > SCI_MAX_WAIT)
            return;
    }

    /* Dummy byte stays in TDR for the whole transfer */
    SCTDR1_REG = 0xFF;

    for(i = 0; i < len; i++) {
        SCSSR1_REG &= (unsigned char)~SSR_TDRE;

        timeout = 0;
        do {
            status = SCSSR1_REG;

            if(status & SSR_ORER)
                SCSSR1_REG &= (unsigned char)~SSR_ORER;

            if(++timeout > SCI_MAX_WAIT)
                return;
        } while(!(status & SSR_RDRF));

        buf[i] = bit_reverse8(SCRDR1_REG);
        SCSSR1_REG &= (unsigned char)~SSR_RDRF;
    }
}

/* ===== SPI Ops Table ===== */

const w5500_spi_ops_t dc_w5500_sci_spi_ops = {
    dc_w5500_sci_spi_init,
    dc_w5500_sci_spi_shutdown,
    dc_w5500_sci_spi_cs_assert,
    dc_w5500_sci_spi_cs_deassert,
    dc_w5500_sci_spi_write_byte,
    dc_w5500_sci_spi_read_byte,
    dc_w5500_sci_spi_write_data,
    dc_w5500_sci_spi_read_data,
};
