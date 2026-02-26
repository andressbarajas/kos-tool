/* client/gamecube/net/bba.c */
/*
 * GameCube Broadband Adapter (BBA) driver.
 *
 * The GC BBA uses a custom ASIC accessed via EXI Channel 0, Device 2.
 * Register access is done through EXI immediate transfers:
 *   - Read:  send (reg << 8), then read N bytes
 *   - Write: send ((0x4000 | reg) << 8) | data, 4 bytes
 *
 * Bulk data (packet read/write) uses EXI DMA transfers.
 *
 * The BBA has 16KB of internal SRAM organized as 256-byte pages:
 *   Pages 0x00-0x09: TX buffer
 *   Pages 0x0A-0x3F: RX buffer
 */

#include <string.h>
#include "bba.h"
#include "packet.h"
#include "net.h"
#include "dcload.h"
#include "dhcp.h"
#include <kosload/target.h>
#include "../exi.h"
#include "../cache.h"
#include <kosload/screensaver.h>

adapter_t adapter_bba = {
    "Broadband Adapter (GC BBA)",
    { 0 },      /* MAC address */
    { 0 },      /* 2-byte alignment pad */
    bba_detect,
    bba_init,
    bba_start,
    bba_stop,
    bba_loop,
    bba_tx
};

/* ===== Low-level EXI register access ===== */

static void bba_select(void)
{
    exi_select(BBA_EXI_CHANNEL, BBA_EXI_DEVICE, EXI_CLK_32MHZ);
}

static void bba_deselect(void)
{
    exi_deselect(BBA_EXI_CHANNEL);
}

/* Read an 8-bit register */
static unsigned char bba_in8(unsigned int reg)
{
    unsigned int val;
    bba_select();
    exi_imm(BBA_EXI_CHANNEL, reg << 8, 4, 1);       /* write cmd */
    val = exi_imm(BBA_EXI_CHANNEL, 0, 1, 0);         /* read 1 byte */
    bba_deselect();
    return (val >> 24) & 0xFF;
}

/* Write an 8-bit register */
static void bba_out8(unsigned int reg, unsigned char val)
{
    bba_select();
    exi_imm(BBA_EXI_CHANNEL, ((0x4000 | reg) << 8) | (val << 0), 4, 1);
    bba_deselect();
}

/* Read a 16-bit register (big-endian) */
static unsigned short bba_in16(unsigned int reg)
{
    unsigned int val;
    bba_select();
    exi_imm(BBA_EXI_CHANNEL, reg << 8, 4, 1);
    val = exi_imm(BBA_EXI_CHANNEL, 0, 2, 0);
    bba_deselect();
    return (val >> 16) & 0xFFFF;
}

/* Read bulk data from a register via DMA.
 * Buffer must be 32-byte aligned. */
static void bba_ins(unsigned int reg, void *buf, int len)
{
    bba_select();
    exi_imm(BBA_EXI_CHANNEL, reg << 8, 4, 1);
    exi_dma(BBA_EXI_CHANNEL, buf, len, 0);  /* DMA read */
    bba_deselect();

    /* Invalidate D-cache over the DMA'd region */
    cache_flush_dc(buf, len);
}

/* Write bulk data to a register via DMA.
 * Buffer must be 32-byte aligned. */
static void __attribute__((unused)) bba_outs(unsigned int reg, const void *buf, int len)
{
    /* Flush D-cache so DMA reads correct data */
    cache_flush_dc(buf, len);

    bba_select();
    exi_imm(BBA_EXI_CHANNEL, (0x4000 | reg) << 8, 4, 1);
    exi_dma(BBA_EXI_CHANNEL, (void *)buf, len, 1);  /* DMA write */
    bba_deselect();
}

/* Write bulk data to a register byte-by-byte via immediate transfers.
 * Use this for small or unaligned transfers. */
static void bba_outs_imm(unsigned int reg, const unsigned char *buf, int len)
{
    int i;
    bba_select();
    exi_imm(BBA_EXI_CHANNEL, (0x4000 | reg) << 8, 4, 1);
    for (i = 0; i < len; i++) {
        exi_imm(BBA_EXI_CHANNEL, (unsigned int)buf[i] << 24, 1, 1);
    }
    bba_deselect();
}

/* Read bulk data byte-by-byte via immediate transfers. */
static void bba_ins_imm(unsigned int reg, unsigned char *buf, int len)
{
    int i;
    bba_select();
    exi_imm(BBA_EXI_CHANNEL, reg << 8, 4, 1);
    for (i = 0; i < len; i++) {
        unsigned int val = exi_imm(BBA_EXI_CHANNEL, 0, 1, 0);
        buf[i] = (val >> 24) & 0xFF;
    }
    bba_deselect();
}

/* ===== BBA initialization ===== */

static void bba_reset(void)
{
    bba_out8(BBA_NCRA, BBA_NCRA_RESET);

    /* Wait for reset to complete */
    int timeout = 10000;
    while ((bba_in8(BBA_NCRA) & BBA_NCRA_RESET) && timeout > 0)
        timeout--;
}

int bba_detect(void)
{
    uint32_t id = exi_get_id(BBA_EXI_CHANNEL, BBA_EXI_DEVICE);

    if (id == BBA_EXI_ID) {
        global_bg_color = BBA_BG_COLOR;
        installed_adapter = BBA_MODEL;
        return 0;
    }

    return -1;
}

int bba_init(void)
{
    int i;

    /* Reset the BBA */
    bba_reset();

    /* Wait for the BBA to be ready */
    i = 10000;
    while (!(bba_in8(BBA_NWAYS) & BBA_NWAYS_LS) && i > 0)
        i--;

    /* Read MAC address from the BBA */
    adapter_bba.mac[0] = bba_in8(BBA_NAFR_PAR0);
    adapter_bba.mac[1] = bba_in8(BBA_NAFR_PAR1);
    adapter_bba.mac[2] = bba_in8(BBA_NAFR_PAR2);
    adapter_bba.mac[3] = bba_in8(BBA_NAFR_PAR3);
    adapter_bba.mac[4] = bba_in8(BBA_NAFR_PAR4);
    adapter_bba.mac[5] = bba_in8(BBA_NAFR_PAR5);

    /* Configure interrupt mask: enable RX and TX interrupts */
    bba_out8(BBA_IMR, BBA_IR_RI | BBA_IR_TI);

    /* Set up TX buffer boundaries */
    bba_out8(BBA_LTPS, BBA_TX_END_PAGE);

    /* Set up RX buffer boundaries */
    bba_out8(BBA_LRPS, BBA_RX_START_PAGE);
    bba_out8(BBA_RHBP, BBA_RX_END_PAGE + 1);

    /* Set initial RX read pointer to start of RX area */
    bba_out8(BBA_RRP, BBA_RX_START_PAGE);
    bba_out8(BBA_RRP + 1, 0x00);

    /* Enable auto-negotiation */
    bba_out8(BBA_NWAYC, BBA_NWAYC_ANE | BBA_NWAYC_ANS_RA);

    /* Enable receive: accept broadcast + physical match */
    bba_out8(BBA_NCRB, BBA_NCRB_AB);

    return 0;
}

void bba_start(void)
{
    /* Enable receiving */
    unsigned char ncra = bba_in8(BBA_NCRA);
    bba_out8(BBA_NCRA, ncra | BBA_NCRA_SR);
}

void bba_stop(void)
{
    /* Disable receiving */
    unsigned char ncra = bba_in8(BBA_NCRA);
    bba_out8(BBA_NCRA, ncra & ~BBA_NCRA_SR);
}

/* ===== TX ===== */

int bba_tx(unsigned char *pkt, int len)
{
    /* Minimum Ethernet frame size (without CRC).
     * Zero-pad to prevent leaking prior packet data (EtherLeak). */
    if (len < 60) {
        memset(pkt + len, 0, 60 - len);
        len = 60;
    }

    /* Write packet data to TX FIFO.
     * Use immediate mode for the transfer since packet data may not be
     * 32-byte aligned (it's at raw_pkt_buf + 2). */
    bba_outs_imm(BBA_WRTXFIFOD, pkt, len);

    /* Set TX length and start transmit */
    bba_out8(BBA_NCRA, bba_in8(BBA_NCRA) | BBA_NCRA_ST0);

    /* Wait for transmit complete */
    int timeout = 100000;
    while (!(bba_in8(BBA_IR) & BBA_IR_TI) && timeout > 0)
        timeout--;

    /* Clear TX interrupt */
    bba_out8(BBA_IR, BBA_IR_TI);

    return 1;
}

/* ===== RX ===== */

/* DMA read buffer (must be 32-byte aligned for EXI DMA) */
static __attribute__((aligned(32))) unsigned char rx_dma_buf[RAW_RX_PKT_BUF_SIZE];

static int bba_rx(void)
{
    int processed = 0;
    unsigned char cur_page, rwp_page;
    unsigned short pkt_len;

    while (1) {
        /* Read current write and read pointers */
        rwp_page = (unsigned char)(bba_in16(BBA_RWP) >> 8);
        cur_page = (unsigned char)(bba_in16(BBA_RRP) >> 8);

        /* No more packets if read pointer equals write pointer */
        if (cur_page == rwp_page)
            break;

        /* Read the 4-byte RX header from the current page.
         * The header contains: next_page, len_lo, len_hi, status. */
        bba_rx_header_t hdr;
        bba_ins_imm(cur_page << 8, (unsigned char *)&hdr, 4);

        pkt_len = ((unsigned short)hdr.packet_len_hi << 8) | hdr.packet_len_lo;

        if (pkt_len > RX_PKT_BUF_SIZE || pkt_len < 14) {
            /* Invalid packet — advance to next page */
            bba_out8(BBA_RRP, hdr.next_page);
            bba_out8(BBA_RRP + 1, 0x00);
            continue;
        }

        /* Read packet data (skip 4-byte header).
         * Read into the aligned DMA buffer, then copy to current_pkt
         * (which has the 2-byte offset for alignment). */
        unsigned int read_len = (pkt_len + 31) & ~31;  /* Align to 32 for DMA */
        if (read_len > RAW_RX_PKT_BUF_SIZE)
            read_len = RAW_RX_PKT_BUF_SIZE;

        bba_ins((cur_page << 8) | 4, rx_dma_buf, read_len);
        memcpy(raw_current_pkt + 2, rx_dma_buf, pkt_len);

        /* Advance RX read pointer to next packet */
        bba_out8(BBA_RRP, hdr.next_page);
        bba_out8(BBA_RRP + 1, 0x00);

        /* Process the packet */
        process_pkt(current_pkt);

        processed++;
    }

    /* Clear RX interrupt */
    bba_out8(BBA_IR, BBA_IR_RI);

    return processed;
}

/* ===== Main loop ===== */

void bba_loop(int is_main_loop)
{
    const target_ops_t *t = target_get_ops();
    uint64_t last_sec_tick = 0;
    unsigned int loop_secs_elapsed = 0;

    if (is_main_loop)
    {
        if (!(booted || running))
            disp_info();
    }

    if (timeout_loop > 0)
    {
        last_sec_tick = t->get_ticks();
    }

    while (!escape_loop)
    {
        /* Check for received packets */
        unsigned char ir = bba_in8(BBA_IR);

        if (ir & BBA_IR_RI)
        {
            bba_rx();
        }

        /* Handle RX buffer full */
        if (ir & BBA_IR_RBFI)
        {
            bba_out8(BBA_IR, BBA_IR_RBFI);
            bba_rx();
        }

        /* Handle errors */
        if (ir & (BBA_IR_REI | BBA_IR_TEI | BBA_IR_FIFOEI | BBA_IR_BUSEI))
        {
            bba_out8(BBA_IR, ir & (BBA_IR_REI | BBA_IR_TEI | BBA_IR_FIFOEI | BBA_IR_BUSEI));
        }

        /* Check for link change.  The BBA has no link change interrupt,
         * so poll NWAYS link status.  Matches the DC RTL8139/LAN adapter
         * pattern: handle link loss in the loop. */
        if (__builtin_expect(!(bba_in8(BBA_NWAYS) & BBA_NWAYS_LS), 0))
        {
            screensaver_wake();

            if (booted && !running)
                disp_status("link change...");

            while (!(bba_in8(BBA_NWAYS) & BBA_NWAYS_LS))
                ;

            if (booted && !running)
                disp_status("idle...");

            /* If we were waiting in a DHCP timeout loop when link
             * changed, timeout immediately so we can retry. */
            if (timeout_loop > 0)
            {
                dhcp_attempts = 0;
                timeout_loop = -1;
                escape_loop = 1;
            }
        }

        if (is_main_loop)
        {
            dhcp_poll();
            screensaver_poll();
        }

        /* Timeout handling for DHCP */
        if (timeout_loop > 0)
        {
            uint64_t now = t->get_ticks();
            if ((now - last_sec_tick) >= t->ticks_per_second) {
                last_sec_tick = now;
                loop_secs_elapsed++;
                if (dhcp_attempts > 1)
                {
                    disp_dhcp_attempts_count();
                    disp_dhcp_next_attempt(timeout_loop - loop_secs_elapsed + 1);
                }
                if (loop_secs_elapsed > (unsigned int)timeout_loop)
                {
                    timeout_loop = -1;
                    escape_loop = 1;
                }
            }
        }
    }
    escape_loop = 0;
}
