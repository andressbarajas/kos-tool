/* client/gamecube/net/enc28j60.h */
/*
 * Microchip ENC28J60 Ethernet controller driver for GameCube.
 *
 * The ENC28J60 is a standalone 10Base-T Ethernet controller with SPI
 * interface. On GameCube it is accessed via the EXI bus in memory card
 * slots (GCNet) or Serial Port 2 (ETH2GC).
 *
 * Device ID: 0xFA050000
 *
 * Reference: Microchip DS39662E datasheet, DS80349C errata.
 */

#ifndef __ENC28J60_H__
#define __ENC28J60_H__

#include "adapter.h"

/* ===== EXI Configuration ===== */

#define ENC28J60_EXI_ID     0xFA050000

/* PHY identification (for validation after reset) */
#define ENC28J60_PHID1_EXPECTED  0x0083
#define ENC28J60_PHID2_EXPECTED  0x1400

/* ===== SPI Command Opcodes ===== */

/* Commands are 1 byte: [OP2:OP0][A4:A0]
 * These macros produce the command byte shifted to bits 31:24 for EXI IMM. */
#define ENC28J60_CMD_RCR(reg)   (((0x00 | ((reg) & 0x1F)) << 24))  /* Read Control Register */
#define ENC28J60_CMD_RBM         ((0x3A) << 24)                      /* Read Buffer Memory */
#define ENC28J60_CMD_WCR(reg)   (((0x40 | ((reg) & 0x1F)) << 24))  /* Write Control Register */
#define ENC28J60_CMD_WBM         ((0x7A) << 24)                      /* Write Buffer Memory */
#define ENC28J60_CMD_BFS(reg)   (((0x80 | ((reg) & 0x1F)) << 24))  /* Bit Field Set */
#define ENC28J60_CMD_BFC(reg)   (((0xA0 | ((reg) & 0x1F)) << 24))  /* Bit Field Clear */
#define ENC28J60_CMD_SRC         ((0xFF) << 24)                      /* Soft Reset */

/* ===== Register Address Encoding =====
 *
 * Software register addresses encode bank + address + type:
 *   Bits [4:0] = register address within bank (0x00-0x1F)
 *   Bits [6:5] = bank number (0-3)
 *   Bit  [7]   = 1 if MAC/MII register (requires dummy byte on read)
 *
 * Registers at address 0x1B-0x1F are common to all banks.
 */

#define ENC_BANK(addr)      (((addr) >> 5) & 0x03)
#define ENC_ADDR(addr)      ((addr) & 0x1F)
#define ENC_IS_MAC_MII(addr) ((addr) & 0x80)

/* ===== Bank 0 Registers (ETH) ===== */

#define ERDPTL      0x00
#define ERDPTH      0x01
#define EWRPTL      0x02
#define EWRPTH      0x03
#define ETXSTL      0x04
#define ETXSTH      0x05
#define ETXNDL      0x06
#define ETXNDH      0x07
#define ERXSTL      0x08
#define ERXSTH      0x09
#define ERXNDL      0x0A
#define ERXNDH      0x0B
#define ERXRDPTL    0x0C
#define ERXRDPTH    0x0D
#define ERXWRPTL    0x0E
#define ERXWRPTH    0x0F
#define EDMASTL     0x10
#define EDMASTH     0x11
#define EDMANDL     0x12
#define EDMANDH     0x13
#define EDMADSTL    0x14
#define EDMADSTH    0x15
#define EDMACSL     0x16
#define EDMACSH     0x17

/* ===== Bank 1 Registers (ETH) ===== */

#define EHT0        0x20
#define EHT1        0x21
#define EHT2        0x22
#define EHT3        0x23
#define EHT4        0x24
#define EHT5        0x25
#define EHT6        0x26
#define EHT7        0x27
#define EPMM0       0x28
#define EPMM1       0x29
#define EPMM2       0x2A
#define EPMM3       0x2B
#define EPMM4       0x2C
#define EPMM5       0x2D
#define EPMM6       0x2E
#define EPMM7       0x2F
#define EPMCSL      0x30
#define EPMCSH      0x31
#define EPMOL       0x34
#define EPMOH       0x35
#define ERXFCON     0x38
#define EPKTCNT     0x39

/* ===== Bank 2 Registers (MAC/MII — need dummy byte on read) ===== */

#define MACON1      (0x40 | 0x80)
#define MACON2      (0x41 | 0x80)
#define MACON3      (0x42 | 0x80)
#define MACON4      (0x43 | 0x80)
#define MABBIPG     (0x44 | 0x80)
#define MAIPGL      (0x46 | 0x80)
#define MAIPGH      (0x47 | 0x80)
#define MACLCON1    (0x48 | 0x80)
#define MACLCON2    (0x49 | 0x80)
#define MAMXFLL     (0x4A | 0x80)
#define MAMXFLH     (0x4B | 0x80)
#define MICMD       (0x52 | 0x80)
#define MIREGADR    (0x54 | 0x80)
#define MIWRL       (0x56 | 0x80)
#define MIWRH       (0x57 | 0x80)
#define MIRDL       (0x58 | 0x80)
#define MIRDH       (0x59 | 0x80)

/* ===== Bank 3 Registers ===== */

#define MAADR5      (0x60 | 0x80)
#define MAADR6      (0x61 | 0x80)
#define MAADR3      (0x62 | 0x80)
#define MAADR4      (0x63 | 0x80)
#define MAADR1      (0x64 | 0x80)
#define MAADR2      (0x65 | 0x80)
#define EBSTSD      0x66
#define EBSTCON     0x67
#define EBSTCSL     0x68
#define EBSTCSH     0x69
#define MISTAT      (0x6A | 0x80)
#define EREVID      0x72
#define ECOCON      0x75
#define EFLOCON     0x77
#define EPAUSL      0x78
#define EPAUSH      0x79

/* ===== Common Registers (all banks, 0x1B-0x1F) ===== */

#define EIE         0x1B
#define EIR         0x1C
#define ESTAT       0x1D
#define ECON2       0x1E
#define ECON1       0x1F

/* ===== ECON1 bits ===== */
#define ECON1_BSEL0     (1 << 0)
#define ECON1_BSEL1     (1 << 1)
#define ECON1_RXEN      (1 << 2)
#define ECON1_TXRTS     (1 << 3)
#define ECON1_CSUMEN    (1 << 4)
#define ECON1_DMAST     (1 << 5)
#define ECON1_RXRST     (1 << 6)
#define ECON1_TXRST     (1 << 7)

/* ===== ECON2 bits ===== */
#define ECON2_VRPS      (1 << 3)
#define ECON2_PWRSV     (1 << 5)
#define ECON2_PKTDEC    (1 << 6)
#define ECON2_AUTOINC   (1 << 7)

/* ===== ESTAT bits ===== */
#define ESTAT_CLKRDY    (1 << 0)
#define ESTAT_TXABRT    (1 << 1)
#define ESTAT_RXBUSY    (1 << 2)
#define ESTAT_LATECOL   (1 << 4)
#define ESTAT_INT       (1 << 7)

/* ===== EIR / EIE bits ===== */
#define EIR_RXERIF      (1 << 0)
#define EIR_TXERIF      (1 << 1)
#define EIR_TXIF        (1 << 3)
#define EIR_LINKIF      (1 << 4)
#define EIR_DMAIF       (1 << 5)
#define EIR_PKTIF       (1 << 6)  /* Errata #6: unreliable, use EPKTCNT */

#define EIE_RXERIE      (1 << 0)
#define EIE_TXERIE      (1 << 1)
#define EIE_TXIE        (1 << 3)
#define EIE_LINKIE      (1 << 4)
#define EIE_DMAIE       (1 << 5)
#define EIE_PKTIE       (1 << 6)
#define EIE_INTIE       (1 << 7)

/* ===== ERXFCON bits ===== */
#define ERXFCON_BCEN    (1 << 0)    /* Broadcast filter enable */
#define ERXFCON_MCEN    (1 << 1)    /* Multicast filter enable */
#define ERXFCON_HTEN    (1 << 2)    /* Hash table filter enable */
#define ERXFCON_MPEN    (1 << 3)    /* Magic packet filter enable */
#define ERXFCON_PMEN    (1 << 4)    /* Pattern match filter enable */
#define ERXFCON_CRCEN   (1 << 5)    /* CRC check enable */
#define ERXFCON_ANDOR   (1 << 6)    /* AND/OR filter select */
#define ERXFCON_UCEN    (1 << 7)    /* Unicast filter enable */

/* ===== MACON1 bits ===== */
#define MACON1_MARXEN   (1 << 0)    /* MAC receive enable */
#define MACON1_PASSALL  (1 << 1)
#define MACON1_RXPAUS   (1 << 2)
#define MACON1_TXPAUS   (1 << 3)

/* ===== MACON3 bits ===== */
#define MACON3_FULDPX   (1 << 0)
#define MACON3_FRMLNEN  (1 << 1)
#define MACON3_HFRMLEN  (1 << 2)
#define MACON3_PHDRLEN  (1 << 3)
#define MACON3_TXCRCEN  (1 << 4)
#define MACON3_PADCFG0  (1 << 5)
#define MACON3_PADCFG1  (1 << 6)
#define MACON3_PADCFG2  (1 << 7)

/* ===== MACON4 bits ===== */
#define MACON4_NOBKOFF  (1 << 4)
#define MACON4_BPEN     (1 << 5)
#define MACON4_DEFER    (1 << 6)

/* ===== MICMD bits ===== */
#define MICMD_MIIRD     (1 << 0)
#define MICMD_MIISCAN   (1 << 1)

/* ===== MISTAT bits ===== */
#define MISTAT_BUSY     (1 << 0)
#define MISTAT_SCAN     (1 << 1)
#define MISTAT_NVALID   (1 << 2)

/* ===== PHY Register Addresses ===== */
#define PHCON1      0x00
#define PHSTAT1     0x01
#define PHID1       0x02
#define PHID2       0x03
#define PHCON2      0x10
#define PHSTAT2     0x11
#define PHIE        0x12
#define PHIR        0x13
#define PHLCON      0x14

/* ===== PHY register bits ===== */
#define PHCON1_PDPXMD   (1 << 8)
#define PHCON1_PPWRSV   (1 << 11)
#define PHCON1_PLOOPBK  (1 << 14)
#define PHCON1_PRST     (1 << 15)

#define PHCON2_HDLDIS   (1 << 8)
#define PHCON2_JABBER   (1 << 10)
#define PHCON2_TXDIS    (1 << 13)
#define PHCON2_FRCLINK  (1 << 14)

#define PHSTAT2_PLRITY  (1 << 5)
#define PHSTAT2_DPXSTAT (1 << 9)
#define PHSTAT2_LSTAT   (1 << 10)
#define PHSTAT2_COLSTAT (1 << 11)
#define PHSTAT2_RXSTAT  (1 << 12)
#define PHSTAT2_TXSTAT  (1 << 13)

#define PHIE_PGEIE      (1 << 1)
#define PHIE_PLNKIE     (1 << 4)

/* ===== Memory Layout =====
 *
 * ENC28J60 has 8KB SRAM (0x0000-0x1FFF).
 * Errata #5: ERXST MUST be 0x0000.
 *
 *   0x0000-0x0FFF: RX buffer (4096 bytes)
 *   0x1000-0x1FFF: TX buffer (4096 bytes)
 */
#define ENC_RX_START    0x0000
#define ENC_RX_END      0x0FFF  /* Must be odd (errata #14) */
#define ENC_TX_START    0x1000
#define ENC_MAX_FRAME   1518

/* ===== Adapter driver functions ===== */

int  enc28j60_detect(void);
void enc28j60_get_exi_location(int *channel, int *device);
int  enc28j60_init(void);
void enc28j60_start(void);
void enc28j60_stop(void);
void enc28j60_loop(bool is_main_loop);
int  enc28j60_tx(unsigned char *pkt, int len);

extern adapter_t adapter_enc28j60;

#endif /* __ENC28J60_H__ */
