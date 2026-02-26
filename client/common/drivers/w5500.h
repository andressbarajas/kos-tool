/* client/common/drivers/w5500.h */
/*
 * WIZnet W5500 Ethernet controller driver for kos-tool.
 *
 * The W5500 is a hardwired TCP/IP embedded Ethernet controller accessed
 * via SPI. We use it in MACRAW mode (Socket 0) for raw Ethernet frames.
 *
 * Platform-specific SPI backends provide a w5500_spi_ops_t struct:
 *   - DC: SCIF bit-bang SPI
 *   - GC: EXI SPI
 *
 * Based on the KallistiOS W5500 network driver by SWAT (Ruslan Rostovtsev):
 *   https://github.com/KallistiOS/KallistiOS/pull/1261
 *
 * Reference: WIZnet W5500 datasheet v1.1.0
 */

#ifndef __W5500_H__
#define __W5500_H__

#include <stdbool.h>

/* Forward declaration — actual definition in platform's net/adapter.h */
struct adapter_s;

/* ===== SPI Abstraction ===== */

/*
 * Platform SPI operations. Each platform provides an implementation.
 * All transfers are MSB-first, SPI mode 0 (CPOL=0, CPHA=0).
 */
typedef struct {
    int  (*init)(void);                             /* Initialize SPI hardware */
    void (*shutdown)(void);                         /* Shutdown SPI hardware */
    void (*cs_assert)(void);                        /* Assert chip select (active low) */
    void (*cs_deassert)(void);                      /* Deassert chip select */
    void (*write_byte)(unsigned char val);           /* Write one byte */
    unsigned char (*read_byte)(void);                /* Read one byte */
    void (*write_data)(const unsigned char *buf, int len);  /* Write multiple bytes */
    void (*read_data)(unsigned char *buf, int len);         /* Read multiple bytes */
} w5500_spi_ops_t;

/* ===== W5500 Block Select Codes ===== */

#define W5500_COMMON_BLOCK  0x00    /* Common registers */
#define W5500_S0_REG_BLOCK  0x01    /* Socket 0 registers */
#define W5500_S0_TX_BLOCK   0x02    /* Socket 0 TX buffer */
#define W5500_S0_RX_BLOCK   0x03    /* Socket 0 RX buffer */

/* ===== Common Register Addresses ===== */

#define W5500_MR            0x0000  /* Mode Register */
#define W5500_SHAR          0x0009  /* Source Hardware Address (MAC, 6 bytes) */
#define W5500_PHYCFGR       0x002E  /* PHY Configuration Register */
#define W5500_VERSIONR      0x0039  /* Chip Version Register (should read 0x04) */

/* ===== Socket 0 Register Addresses ===== */

#define W5500_Sn_MR         0x0000  /* Socket Mode */
#define W5500_Sn_CR         0x0001  /* Socket Command */
#define W5500_Sn_IR         0x0002  /* Socket Interrupt */
#define W5500_Sn_SR         0x0003  /* Socket Status */
#define W5500_Sn_PORT       0x0004  /* Socket Source Port (2 bytes) */
#define W5500_Sn_RXBUF_SIZE 0x001E  /* RX Buffer Size (KB) */
#define W5500_Sn_TXBUF_SIZE 0x001F  /* TX Buffer Size (KB) */
#define W5500_Sn_TX_FSR     0x0020  /* TX Free Size (2 bytes) */
#define W5500_Sn_TX_RD      0x0022  /* TX Read Pointer (2 bytes) */
#define W5500_Sn_TX_WR      0x0024  /* TX Write Pointer (2 bytes) */
#define W5500_Sn_RX_RSR     0x0026  /* RX Received Size (2 bytes) */
#define W5500_Sn_RX_RD      0x0028  /* RX Read Pointer (2 bytes) */
#define W5500_Sn_RX_WR      0x002A  /* RX Write Pointer (2 bytes) */
#define W5500_Sn_IMR        0x002C  /* Socket Interrupt Mask */

/* ===== Command Register Values ===== */

#define W5500_CR_OPEN       0x01    /* Open socket */
#define W5500_CR_CLOSE      0x10    /* Close socket */
#define W5500_CR_SEND       0x20    /* Send data */
#define W5500_CR_RECV       0x40    /* Receive complete */

/* ===== Mode Register Bits ===== */

#define W5500_MR_RST        0x80    /* Software reset */

/* ===== Socket Mode Bits ===== */

#define W5500_Sn_MR_MACRAW  0x04    /* MACRAW mode */
#define W5500_Sn_MR_MFEN    0x80    /* MAC filter enable */

/* ===== Socket Status Values ===== */

#define W5500_SOCK_MACRAW   0x42    /* Socket in MACRAW mode */

/* ===== Socket Interrupt Bits ===== */

#define W5500_Sn_IR_SENDOK  0x10    /* Send complete */
#define W5500_Sn_IR_RECV    0x04    /* Data received */

/* ===== SPI Control Phase Bits ===== */

#define W5500_SPI_READ      (0x00 << 2)  /* Read access */
#define W5500_SPI_WRITE     (0x01 << 2)  /* Write access */
#define W5500_SPI_VDM       0x00         /* Variable Data Length Mode */

/* ===== W5500 Chip Version ===== */

#define W5500_VERSION_EXPECTED  0x04

/* ===== Display constants =====
 *
 * W5500_BG_COLOR and W5500_MODEL are defined per-platform in dcload.h.
 * The w5500.c driver uses them via dcload.h includes.
 */

/* ===== Adapter driver functions ===== */

/*
 * Initialize the W5500 driver with the given SPI backend.
 * Must be called before w5500_detect().
 */
void w5500_set_spi_ops(const w5500_spi_ops_t *ops);

int  w5500_detect(void);
int  w5500_init(void);
void w5500_start(void);
void w5500_stop(void);
void w5500_loop(bool is_main_loop);
int  w5500_tx(unsigned char *pkt, int len);

/* Defined in w5500.c — use after including platform adapter.h */
/* extern adapter_t adapter_w5500; */

#endif /* __W5500_H__ */
