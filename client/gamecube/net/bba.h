/* client/gamecube/net/bba.h */
/*
 * GameCube Broadband Adapter (BBA) driver header.
 *
 * The GC BBA uses a custom ASIC wrapper around an RTL8139-like core,
 * accessed via EXI Channel 0, Device 2. The register interface is
 * completely different from the DC BBA's GAPS PCI bridge approach.
 */

#ifndef __BBA_H__
#define __BBA_H__

#include "adapter.h"

/* ===== EXI Configuration ===== */

#define BBA_EXI_CHANNEL  0
#define BBA_EXI_DEVICE   2
#define BBA_EXI_ID       0x04020200

/* ===== BBA Register Addresses ===== */

/* Control registers */
#define BBA_NCRA        0x00    /* Network Control Register A */
#define BBA_NCRB        0x01    /* Network Control Register B */
#define BBA_LTPS        0x04    /* Last TX Page Start */
#define BBA_LRPS        0x05    /* Last RX Page Start */
#define BBA_IMR         0x08    /* Interrupt Mask Register */
#define BBA_IR          0x09    /* Interrupt Register */

/* Buffer pointers */
#define BBA_BP          0x0A    /* Boundary Page */
#define BBA_TLBP        0x0C    /* TX Low Boundary Page */
#define BBA_TWP         0x0E    /* TX Write Pointer (16-bit) */
#define BBA_TRP         0x12    /* TX Read Pointer (16-bit) */
#define BBA_RWP         0x16    /* RX Write Pointer (16-bit) */
#define BBA_RRP         0x18    /* RX Read Pointer (16-bit) */
#define BBA_RHBP        0x1A    /* RX High Boundary Page */

/* MAC address registers */
#define BBA_NAFR_PAR0   0x20    /* Physical Address Register 0 (MAC byte 0) */
#define BBA_NAFR_PAR1   0x21
#define BBA_NAFR_PAR2   0x22
#define BBA_NAFR_PAR3   0x23
#define BBA_NAFR_PAR4   0x24
#define BBA_NAFR_PAR5   0x25

/* Link/PHY registers */
#define BBA_NWAYC       0x30    /* NWAY Control */
#define BBA_NWAYS       0x31    /* NWAY Status */
#define BBA_GCA         0x32    /* GCA register */

/* Misc registers */
#define BBA_MISC        0x3D    /* Misc */
#define BBA_TXFIFOCNT   0x3E    /* TX FIFO Count (16-bit) */
#define BBA_WRTXFIFOD   0x48    /* Write TX FIFO Data */
#define BBA_MISC2       0x50    /* Misc 2 */
#define BBA_SI_ACTRL    0x5C    /* Serial Interface Active Control */
#define BBA_SI_STATUS   0x5D    /* Serial Interface Status */
#define BBA_SI_ACTRL2   0x60    /* Serial Interface Active Control 2 */

/* ===== BBA_NCRA bits ===== */
#define BBA_NCRA_RESET  (1 << 0)  /* Software reset */
#define BBA_NCRA_ST0    (1 << 1)  /* Start transmit command */
#define BBA_NCRA_ST1    (1 << 2)  /* Start transmit (link change) */
#define BBA_NCRA_SR     (1 << 3)  /* Start receive */

/* ===== BBA_NCRB bits ===== */
#define BBA_NCRB_PR     (1 << 0)  /* Promiscuous mode */
#define BBA_NCRB_CA     (1 << 1)  /* Capture effect */
#define BBA_NCRB_PM     (1 << 2)  /* Pass multicast */
#define BBA_NCRB_PB     (1 << 3)  /* Pass bad frame */
#define BBA_NCRB_AB     (1 << 4)  /* Accept broadcast */

/* ===== BBA_IR / BBA_IMR bits ===== */
#define BBA_IR_FRAGI    (1 << 0)  /* Fragment counter interrupt */
#define BBA_IR_RI       (1 << 1)  /* Receive interrupt */
#define BBA_IR_TI       (1 << 2)  /* Transmit interrupt */
#define BBA_IR_REI      (1 << 3)  /* Receive error interrupt */
#define BBA_IR_TEI      (1 << 4)  /* Transmit error interrupt */
#define BBA_IR_FIFOEI   (1 << 5)  /* FIFO error interrupt */
#define BBA_IR_BUSEI    (1 << 6)  /* Bus error interrupt */
#define BBA_IR_RBFI     (1 << 7)  /* RX buffer full interrupt */

/* ===== BBA_NWAYC bits ===== */
#define BBA_NWAYC_FD    (1 << 0)  /* Full duplex */
#define BBA_NWAYC_PS100 (1 << 1)  /* Port speed 100Mbps */
#define BBA_NWAYC_ANE   (1 << 2)  /* Auto-negotiation enable */
#define BBA_NWAYC_ANS_RA (1 << 3) /* Auto-negotiation restart */

/* ===== BBA_NWAYS bits ===== */
#define BBA_NWAYS_LS    (1 << 0)  /* Link status */
#define BBA_NWAYS_ANCLPT (1 << 1) /* Auto-negotiation complete */
#define BBA_NWAYS_100   (1 << 2)  /* 100Mbps link */
#define BBA_NWAYS_FD    (1 << 3)  /* Full duplex link */

/* ===== Memory layout ===== */

/* BBA has 16KB internal buffer memory, organized as 256-byte pages.
 * Pages 0x00-0x09: TX buffer (2.5KB)
 * Pages 0x0A-0x3F: RX buffer (13.5KB) */
#define BBA_TX_START_PAGE   0x00
#define BBA_TX_END_PAGE     0x09
#define BBA_RX_START_PAGE   0x0A
#define BBA_RX_END_PAGE     0x3F

#define BBA_PAGE_SIZE       256

/* RX packet header (prepended by BBA to each received frame) */
typedef struct {
    unsigned char next_page;    /* Next packet page */
    unsigned char packet_len_lo;
    unsigned char packet_len_hi;
    unsigned char status;       /* Receive status */
} __attribute__((packed)) bba_rx_header_t;

/* Adapter driver functions */
int  bba_detect(void);
int  bba_init(void);
void bba_start(void);
void bba_stop(void);
void bba_loop(bool is_main_loop);
int  bba_tx(unsigned char *pkt, int len);

#endif /* __BBA_H__ */
