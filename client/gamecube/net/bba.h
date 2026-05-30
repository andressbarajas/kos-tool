/* client/gamecube/net/bba.h */
/*
 * GameCube Broadband Adapter (BBA, Nintendo part DOL-015) driver header.
 *
 * Hardware: Macronix MX98730ec-class 10/100 Ethernet ASIC sitting on
 * EXI Channel 0, Device 2.
 */

#ifndef __BBA_H__
#define __BBA_H__

#include "adapter.h"

/* ===== EXI bus location ===== */

#define GCBBA_EXI_CHANNEL    0
#define GCBBA_EXI_DEVICE     2
#define GCBBA_EXI_ID         0x04020200

/* ===== Register addresses =====
 *
 * Multi-byte registers store the low byte at the base address and the
 * high byte at base+1.  The driver writes them as two 8-bit accesses.
 */

/* Network control / interrupt block */
#define GCBBA_NCRA         0x00  /* primary control */
#define GCBBA_NCRB         0x01  /* secondary control (left at 0x52) */
#define GCBBA_IMR          0x08  /* interrupt mask */
#define GCBBA_IR           0x09  /* interrupt status (write-1-clear) */

/* Receive ring pointers (low byte at base, high byte at base+1) */
#define GCBBA_BP           0x0A  /* boundary page */
#define GCBBA_TLBP         0x0C  /* TX low boundary page */
#define GCBBA_RWP          0x16  /* RX write pointer */
#define GCBBA_RRP          0x18  /* RX read pointer */
#define GCBBA_RHBP         0x1A  /* RX high boundary page */

/* Permanent (factory-programmed) MAC address, MSB first */
#define GCBBA_PAR0         0x20
#define GCBBA_PAR1         0x21
#define GCBBA_PAR2         0x22
#define GCBBA_PAR3         0x23
#define GCBBA_PAR4         0x24
#define GCBBA_PAR5         0x25

/* PHY / auto-negotiation block */
#define GCBBA_NWAYC        0x30  /* PHY mode-select */
#define GCBBA_NWAYS        0x31  /* PHY status (link / negotiated speed) */
#define GCBBA_GCA          0x32  /* general PHY config */

/* TX FIFO data port (write-only) */
#define GCBBA_WRTXFIFOD     0x48

/* Misc / serial-interface control block */
#define GCBBA_MISC2         0x50
#define GCBBA_SI_ACTRL      0x5C
#define GCBBA_SI_STATUS     0x5D
#define GCBBA_SI_ACTRL2     0x60

/* ===== NCRA bits ===== */
#define GCBBA_NCRA_RST      (1 << 0)  /* assert/deassert soft reset */
#define GCBBA_NCRA_ST0      (1 << 1)  /* start TX from page list */
#define GCBBA_NCRA_ST1      (1 << 2)  /* start TX directly from FIFO */
#define GCBBA_NCRA_RX_EN    (1 << 3)  /* enable receiver */

/* ===== Interrupt bits (shared by IR and IMR) ===== */
#define GCBBA_INT_RX        (1 << 1)  /* RX done */
#define GCBBA_INT_TX        (1 << 2)  /* TX done */
#define GCBBA_INT_RX_ERR    (1 << 3)
#define GCBBA_INT_TX_ERR    (1 << 4)
#define GCBBA_INT_FIFO_ERR  (1 << 5)
#define GCBBA_INT_BUS_ERR   (1 << 6)
#define GCBBA_INT_RX_FULL   (1 << 7)  /* RX SRAM page-list exhausted */

/* ===== NWAYC bits ===== */
#define GCBBA_NWAYC_FD      (1 << 0)  /* full-duplex select */
#define GCBBA_NWAYC_PS100   (1 << 1)  /* 100 Mbps select */

/* ===== NWAYS link-up gate =====
 * Bits 0 and 1 of NWAYS report 10 Mbps and 100 Mbps link status; either
 * being set means the chip has settled on a usable link.  The driver
 * only needs the composite, so the individual bits aren't named here. */
#define GCBBA_NWAYS_LINK_UP 0x03

/* ===== Adapter driver entry points ===== */
int  bba_detect(void);
int  bba_init(void);
void bba_start(void);
void bba_stop(void);
void bba_loop(bool is_main_loop);
int  bba_tx(unsigned char *pkt, int len);

#endif /* __BBA_H__ */
