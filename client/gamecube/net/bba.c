/* client/gamecube/net/bba.c */
/*
 * GameCube Broadband Adapter (DOL-015) driver.
 *
 * The BBA is a Macronix MX98730ec-class Ethernet ASIC reached over EXI
 * Channel 0, Device 2.  Two distinct on-bus command formats are used:
 *
 *   * 4-byte register access (this driver's normal path):
 *         byte 0 = direction (0x80 read, 0xC0 write)
 *         byte 1 = high address (page for SRAM, 0 for register space)
 *         byte 2 = low address (register/offset)
 *         byte 3 = pad (data is sent as a separate transfer for writes)
 *
 *   * 2-byte "chip-port" commands used only during init / reset.
 *     These open a side-channel to the ASIC's wake-up and chip-magic
 *     registers; see bba_chipport_*() below.
 *
 * Bulk reads/writes of the BBA's 16 KiB SRAM use EXI DMA.  SRAM is
 * organised as 256-byte pages: pages 0x00-0x09 hold the TX buffer and
 * pages 0x0A-0x3F hold the RX ring.
 */

#include <string.h>

#include <kosload/target.h>
#include <kosload/screensaver.h>

#include "bba.h"
#include "packet.h"
#include "net.h"
#include "dcload.h"
#include "dhcp.h"
#include "../exi.h"
#include "../cache.h"

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

/* ===== Low-level EXI register access =====
 *
 * The 4-byte register-access cmd word packs as:
 *
 *   bits 31:24  direction (0x80 read, 0xC0 write)
 *   bits 23:16  high address byte (SRAM page or 0 for register file)
 *   bits 15:8   low address byte (register offset)
 *   bits  7:0   pad / write-data (0 here; data is sent separately)
 *
 * A combined word is therefore (dir | (addr << 8)) where dir is one of
 * the two direction constants below and addr is the 16-bit target. */

#define GCBBA_CMD_READ      0x80000000u
#define GCBBA_CMD_WRITE     0xC0000000u
#define GCBBA_CMD(dir, addr) ((dir) | (((addr) & 0xFFFFu) << 8))

static void bba_select(void) {
    exi_select(GCBBA_EXI_CHANNEL, GCBBA_EXI_DEVICE, EXI_CLK_32MHZ);
}

static void bba_deselect(void) {
    exi_deselect(GCBBA_EXI_CHANNEL);
}

/* Read an 8-bit register */
static unsigned char bba_in8(unsigned int reg) {
    unsigned int val;
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, GCBBA_CMD(GCBBA_CMD_READ, reg), 4, EXI_IMM_WRITE);
    val = exi_imm(GCBBA_EXI_CHANNEL, 0, 1, EXI_IMM_READ);
    bba_deselect();
    return (val >> 24) & 0xFF;
}

/* Write an 8-bit register */
static void bba_out8(unsigned int reg, unsigned char val) {
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, GCBBA_CMD(GCBBA_CMD_WRITE, reg), 4, EXI_IMM_WRITE);
    exi_imm(GCBBA_EXI_CHANNEL, (unsigned int)val << 24, 1, EXI_IMM_WRITE);
    bba_deselect();
}

/* Read a 16-bit register (big-endian) */
static unsigned short bba_in16(unsigned int reg) {
    unsigned int val;
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, GCBBA_CMD(GCBBA_CMD_READ, reg), 4, EXI_IMM_WRITE);
    val = exi_imm(GCBBA_EXI_CHANNEL, 0, 2, EXI_IMM_READ);
    bba_deselect();
    return (val >> 16) & 0xFFFF;
}

/* Read bulk data from a register via DMA.
 * Buffer must be 32-byte aligned. */
static void bba_ins(unsigned int reg, void *buf, int len) {
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, GCBBA_CMD(GCBBA_CMD_READ, reg), 4, EXI_IMM_WRITE);
    exi_dma(GCBBA_EXI_CHANNEL, buf, len, EXI_DMA_READ);
    bba_deselect();

    /* Invalidate D-cache over the DMA'd region */
    cache_flush_dc(buf, len);
}

/* Write bulk data to a register via DMA.
 * Buffer must be 32-byte aligned.
 * Currently unused — bba_tx inlines the equivalent. */
static void __attribute__((unused)) bba_outs(unsigned int reg, const void *buf, int len)
{
    /* Flush D-cache so DMA reads correct data */
    cache_flush_dc(buf, len);

    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, GCBBA_CMD(GCBBA_CMD_WRITE, reg), 4, EXI_IMM_WRITE);
    exi_dma(GCBBA_EXI_CHANNEL, (void *)buf, len, EXI_DMA_WRITE);
    bba_deselect();
}

/* Write bulk data to a register byte-by-byte via immediate transfers.
 * Use this for small or unaligned transfers.
 * Currently unused — bba_tx inlines the equivalent. */
static void __attribute__((unused)) bba_outs_imm(unsigned int reg, const unsigned char *buf, int len) {
    int i;
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, GCBBA_CMD(GCBBA_CMD_WRITE, reg), 4, EXI_IMM_WRITE);
    for(i = 0; i < len; i++) {
        exi_imm(GCBBA_EXI_CHANNEL, (unsigned int)buf[i] << 24, 1, EXI_IMM_WRITE);
    }
    bba_deselect();
}

/* Read bulk data byte-by-byte via immediate transfers. */
static void bba_ins_imm(unsigned int reg, unsigned char *buf, int len) {
    int i;
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, GCBBA_CMD(GCBBA_CMD_READ, reg), 4, EXI_IMM_WRITE);
    for(i = 0; i < len; i++) {
        unsigned int val = exi_imm(GCBBA_EXI_CHANNEL, 0, 1, EXI_IMM_READ);
        buf[i] = (val >> 24) & 0xFF;
    }
    bba_deselect();
}

/* ===== BBA initialization ===== */

/* Coarse busy-loop calibrated for the GameCube's ~486 MHz PowerPC.
 * Each volatile-asm-bounded iteration is a small handful of cycles, so
 * 100,000 iterations is roughly one millisecond.  Used only for the
 * post-wake-up settling delays the ASIC needs during init. */
static void bba_busy_delay_ms(int ms) {
    int i;
    for(i = 0; i < ms * 100000; i++)
        __asm__ volatile("" ::: "memory");
}

/* Coarse 1us busy-loop helper for the reset settling delays. */
static void bba_busy_delay_us(int us) {
    int i;
    for(i = 0; i < us * 100; i++)
        __asm__ volatile("" ::: "memory");
}

/* "Chip-port" 2-byte cmd format used only by init/reset paths.  Distinct
 * from the 4-byte register access defined above:
 *
 *   cmd 0x4200 + 1-byte data : chip wakeup (0x00) / activate (0xF8)
 *   cmd 0x4400 + 2-byte data : chip-magic 16-bit write
 *   cmd 0x4500 + 1-byte data : chip-magic 8-bit write
 *   cmd 0x0F00 + 1-byte read : chip-status read
 *   cmd 0x0100 + 1-byte read : another chip-status read
 *
 * These open a back-channel to the ASIC's chip-magic and wake-up logic
 * that must be exercised before normal register access becomes
 * effective. */
static void bba_chipport_write1(unsigned char op, unsigned char data) {
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, (unsigned int)op << 24, 2, EXI_IMM_WRITE);
    exi_imm(GCBBA_EXI_CHANNEL, (unsigned int)data << 24, 1, EXI_IMM_WRITE);
    bba_deselect();
}

static void bba_chipport_write2(unsigned char op,
                                unsigned char d0, unsigned char d1) {
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, (unsigned int)op << 24, 2, EXI_IMM_WRITE);
    exi_imm(GCBBA_EXI_CHANNEL,
            ((unsigned int)d0 << 24) | ((unsigned int)d1 << 16),
            2, EXI_IMM_WRITE);
    bba_deselect();
}

static unsigned char bba_chipport_read1(unsigned char op) {
    unsigned int val;
    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL, (unsigned int)op << 24, 2, EXI_IMM_WRITE);
    val = exi_imm(GCBBA_EXI_CHANNEL, 0, 1, EXI_IMM_READ);
    bba_deselect();
    return (val >> 24) & 0xFF;
}

/* The chip wakeup / activate cmd (0x42).  Convenience wrapper. */
static void bba_cmd42(unsigned char data) {
    bba_chipport_write1(0x42, data);
}

static void bba_reset(void) {
    /* Soft-reset sequence: explicitly assert RST, settle, explicitly
     * deassert, settle.  Polling NCRA for the bit to auto-clear is
     * unreliable on this ASIC variant — the bit does not always clear
     * itself, so drive both edges from the host side. */
    bba_out8(GCBBA_NCRA, GCBBA_NCRA_RST);
    bba_busy_delay_us(100);
    bba_out8(GCBBA_NCRA, 0x00);
    bba_busy_delay_us(100);
}

int bba_detect(void) {
    uint32_t id = exi_get_id(GCBBA_EXI_CHANNEL, GCBBA_EXI_DEVICE);

    if(id == GCBBA_EXI_ID) {
        global_bg_color = BBA_BG_COLOR;
        installed_adapter = BBA_MODEL;
        return 0;
    }

    return -1;
}

int bba_init(void) {
    int i;

    /* Bring-up sequence for the BBA ASIC.  Every step here was needed
     * to get the chip to actually deliver received frames into RX SRAM
     * — earlier abbreviated init versions left the chip in a state
     * where link came up but RX never landed.  The SI block, the GCA
     * write, the RX-enable bit, and the NWAYC mode-select are all
     * required. */

    /* 1. Chip wake-up: cmd 0x4200 + 0x00 */
    bba_cmd42(0x00);

    /* 2. SI_ACTRL2 = 0x00 (gate PHY off) + 10ms settle */
    bba_out8(GCBBA_SI_ACTRL2, 0x00);
    bba_busy_delay_ms(10);

    /* 3. Chip-port read at 0x0F (result discarded — chip side-effect) */
    (void)bba_chipport_read1(0x0F);
    bba_busy_delay_us(200);
    bba_busy_delay_ms(10);

    /* 4. Soft-reset NCRA: assert RST, settle, deassert, settle */
    bba_reset();

    /* 5. Chip-port read at 0x01 (result discarded — chip side-effect) */
    (void)bba_chipport_read1(0x01);

    /* 6. Chip-magic config write — pushes a 0xF0D1074E configuration
     *    word via two chip-port commands.  This sets up internal ASIC
     *    state that the SI block writes below depend on. */
    bba_chipport_write2(0x44, 0xD1, 0x07);
    bba_chipport_write1(0x45, 0x4E);

    /* 7. Serial-interface config block (regs 0x58-0x5F).  Without these
     *    the chip's MAC↔EXI bridge stays in a state where received
     *    frames are dropped silently. */
    bba_out8(0x58, 0x80);
    bba_out8(0x59, 0x00);
    bba_out8(0x5A, 0x03);
    bba_out8(0x5B, 0x83);
    bba_out8(GCBBA_SI_ACTRL,  0x32);
    bba_out8(GCBBA_SI_STATUS, 0xFE);
    bba_out8(0x5E, 0x1F);
    bba_out8(0x5F, 0x1F);
    bba_busy_delay_us(100);

    /* 8. Main control config writes.  NCRB=0x52: bits 6:7 act as the
     *    RX interrupt counter on this silicon; 0x52 leaves the counter
     *    in its lowest setting, which gives us per-packet RX interrupts
     *    instead of batched ones. */
    bba_out8(GCBBA_NCRB,      0x52);
    bba_out8(GCBBA_SI_ACTRL2, 0x74);
    bba_out8(0x14,            0x00);
    bba_out8(0x15,            0x06);
    bba_out8(GCBBA_MISC2,     0x80);

    /* 9. TX/RX SRAM page boundaries.
     *    TLBP = 0x0000   TX buffer starts at page 0
     *    BP   = 0x0001   first RX page ("boundary")
     *    RHBP = 0x0010   RX ring upper bound (last page + 1)
     *    RWP/RRP both initialised to page 1 (empty ring). */
    bba_out8(GCBBA_TLBP, 0x00);  bba_out8(GCBBA_TLBP + 1, 0x00);
    bba_out8(GCBBA_BP,   0x01);  bba_out8(GCBBA_BP   + 1, 0x00);
    bba_out8(GCBBA_RHBP, 0x10);  bba_out8(GCBBA_RHBP + 1, 0x00);
    bba_out8(GCBBA_RWP,  0x01);  bba_out8(GCBBA_RWP  + 1, 0x00);
    bba_out8(GCBBA_RRP,  0x01);  bba_out8(GCBBA_RRP  + 1, 0x00);

    /* 10. GCA = 0x08 — general PHY config; written after the buffer
     *     pointers and before enabling RX. */
    bba_out8(GCBBA_GCA, 0x08);

    /* 11. Enable receiver via NCRA's RX_EN bit.
     *     Side-effect on this ASIC: NWAYS link-status bits become
     *     sticky once link comes up, so mid-session cable removal can
     *     no longer be detected.  Accepted tradeoff: RX must work for
     *     the loader to be useful. */
    bba_out8(GCBBA_NCRA, GCBBA_NCRA_RX_EN);

    /* 12. NWAYC: force 100Mbps full-duplex.  Setting FD|PS100 with the
     *     auto-negotiation enable bit clear pins the PHY to 100FD.
     *     Auto-negotiation restart hung on the user's hardware during
     *     long DBIN transfers, so forced mode is preferred. */
    unsigned char preserve = bba_in8(GCBBA_NWAYC) & 0xC0u;
    bba_out8(GCBBA_NWAYC,
                preserve | GCBBA_NWAYC_FD | GCBBA_NWAYC_PS100);

    /* 13. Read the factory-programmed MAC address (PAR0..PAR5). */
    adapter_bba.mac[0] = bba_in8(GCBBA_PAR0);
    adapter_bba.mac[1] = bba_in8(GCBBA_PAR1);
    adapter_bba.mac[2] = bba_in8(GCBBA_PAR2);
    adapter_bba.mac[3] = bba_in8(GCBBA_PAR3);
    adapter_bba.mac[4] = bba_in8(GCBBA_PAR4);
    adapter_bba.mac[5] = bba_in8(GCBBA_PAR5);

    /* 14. Interrupt regs: clear all pending (IR=0xFF, write-1-clear),
     *     enable all sources except FIFO error (IMR=0xDF). */
    bba_out8(GCBBA_IR,  0xFF);
    bba_out8(GCBBA_IMR, 0xDF);

    /* 15. Final activation: cmd 0x4200 + 0xF8 — chip "go live". */
    bba_cmd42(0xF8);

    /* 16. Wait for link to come up.  NWAYS bits 0/1 report 10/100
     *     link-status; either bit set means link is up.
     *
     *     With auto-negotiation restart (which we don't use here) link
     *     can take several seconds to settle on this hardware; in
     *     forced 100FD mode it usually settles much faster, but the
     *     loop is generous enough either way. */
    i = 1000000;
    while(i > 0 && !(bba_in8(GCBBA_NWAYS) & GCBBA_NWAYS_LINK_UP))
        i--;

    return 0;
}

void bba_start(void) {
    /* Enable receiving */
    unsigned char ncra = bba_in8(GCBBA_NCRA);
    bba_out8(GCBBA_NCRA, ncra | GCBBA_NCRA_RX_EN);
}

void bba_stop(void) {
    /* Disable receiving */
    unsigned char ncra = bba_in8(GCBBA_NCRA);
    bba_out8(GCBBA_NCRA, ncra & ~GCBBA_NCRA_RX_EN);
}

/* ===== TX ===== */

/* TX bounce buffer — 32-byte aligned for EXI DMA.  Network-stack
 * packet buffers live at `raw_pkt_buf + 2` (2-byte offset for
 * Ethernet/IP header alignment), so they can't be DMA'd directly.
 * 1536 bytes covers the 1518-byte Ethernet maximum. */
static __attribute__((aligned(32))) unsigned char tx_dma_buf[1536];

int bba_tx(unsigned char *pkt, int len) {
    /* Wait for any previous TX to finish.  GCBBA_INT_TX is W1C — we
     * cleared it after the last TX, so seeing it set means the prior
     * TX completed.  First call: no prior TX, prev_tx_pending is 0
     * so we skip the wait entirely. */
    static int prev_tx_pending = 0;
    if(prev_tx_pending) {
        int wait = 10000;
        while(!(bba_in8(GCBBA_IR) & GCBBA_INT_TX) && wait > 0)
            wait--;
        bba_out8(GCBBA_IR, GCBBA_INT_TX);   /* W1C ack */
    }
    prev_tx_pending = 1;

    /* Pad to Ethernet 60-byte minimum (zero-pad to prevent EtherLeak).
     * Copy to the aligned bounce buffer so we can use EXI DMA without
     * mutating the caller's packet. */
    int tx_len = (len < 60) ? 60 : len;
    if(tx_len > (int)sizeof(tx_dma_buf))
        tx_len = sizeof(tx_dma_buf);
    if(len > tx_len)
        len = tx_len;
    memcpy(tx_dma_buf, pkt, len);
    if(tx_len > len)
        memset(tx_dma_buf + len, 0, tx_len - len);

    /* GameCube EXI DMA hardware requires the transfer length to be a
     * multiple of 32 bytes (per YAGCD §10.3 and HW behavior).  Split
     * the packet:
     *   - DMA the largest 32-byte-aligned prefix
     *   - Send any trailing 0..31 bytes via single-byte EXI_ImmEx
     * Both phases happen within the same select/deselect cycle, so
     * the chip sees one continuous WRTXFIFOD write. */
    int dma_len = tx_len & ~31;          /* round DOWN to 32-aligned */
    int imm_len = tx_len - dma_len;      /* 0..31 trailing bytes */

    if(dma_len > 0)
        cache_flush_dc(tx_dma_buf, dma_len);

    bba_select();
    exi_imm(GCBBA_EXI_CHANNEL,
            GCBBA_CMD(GCBBA_CMD_WRITE, GCBBA_WRTXFIFOD),
            4, EXI_IMM_WRITE);

    if(dma_len > 0)
        exi_dma(GCBBA_EXI_CHANNEL, tx_dma_buf, dma_len, EXI_DMA_WRITE);

    for(int i = 0; i < imm_len; i++)
        exi_imm(GCBBA_EXI_CHANNEL,
                (unsigned int)tx_dma_buf[dma_len + i] << 24,
                1, EXI_IMM_WRITE);

    bba_deselect();

    /* Trigger TX via NCRA's ST1 bit (direct-FIFO TX start).  Edge-
     * triggered: clear both start bits first, then set ST1.  ST0 is
     * the page-list TX path, which we don't use because the FIFO path
     * lets us stream the frame directly through WRTXFIFOD without
     * managing TX SRAM pages. */
    unsigned char ncra = bba_in8(GCBBA_NCRA);
    ncra = (ncra & ~(GCBBA_NCRA_ST0 | GCBBA_NCRA_ST1)) | GCBBA_NCRA_ST1;
    bba_out8(GCBBA_NCRA, ncra);

    /* Fire-and-forget: don't wait for the TX-done interrupt here.  We
     * poll it at the START of the NEXT call so back-to-back TXes
     * serialize without blocking the loop. */
    return 1;
}

/* ===== RX ===== */

/* DMA read buffer (must be 32-byte aligned for EXI DMA) */
static __attribute__((aligned(32))) unsigned char rx_dma_buf[RAW_RX_PKT_BUF_SIZE];

static int bba_rx(void) {
    int processed = 0;
    unsigned char cur_page, rwp_page;
    unsigned short pkt_len;

    while(1) {
        /* Read current write and read pointers */
        rwp_page = (unsigned char)(bba_in16(GCBBA_RWP) >> 8);
        cur_page = (unsigned char)(bba_in16(GCBBA_RRP) >> 8);

        /* No more packets if read pointer equals write pointer */
        if(cur_page == rwp_page)
            break;

        /* Read the 4-byte RX descriptor from the current page.  The
         * ASIC stores the descriptor as a single packed 32-bit word,
         * little-endian:
         *
         *   word = (status         & 0xFF ) << 24
         *        | (packet_length  & 0xFFF) << 12
         *        | (next_page      & 0xFFF)
         *
         * Reading 4 bytes [b0..b3] in memory order, the actual word is
         * (b3<<24) | (b2<<16) | (b1<<8) | b0.  This is empirically the
         * format produced by real BBA hardware. */
        unsigned char hdr_bytes[4];
        bba_ins_imm(cur_page << 8, hdr_bytes, 4);

        unsigned int desc = ((unsigned int)hdr_bytes[3] << 24)
                          | ((unsigned int)hdr_bytes[2] << 16)
                          | ((unsigned int)hdr_bytes[1] <<  8)
                          | ((unsigned int)hdr_bytes[0]);
        unsigned char status_byte = (desc >> 24) & 0xFF;
        pkt_len = (desc >> 12) & 0xFFF;
        unsigned int next_page = desc & 0xFFF;
        (void)status_byte;

        /* RX area is pages 1..0xF (RHBP=0x10).  Skip invalid
         * packets and advance RRP to next valid page. */
        if(pkt_len > RX_PKT_BUF_SIZE || pkt_len < 14 ||
            next_page < 1 || next_page > 0x10) {
            if(next_page < 1 || next_page > 0x10)
                next_page = (cur_page + 1 > 0x0F) ? 0x01 : (cur_page + 1);
            bba_out8(GCBBA_RRP,     (unsigned char)next_page);
            bba_out8(GCBBA_RRP + 1, 0x00);
            continue;
        }

        /* Read packet data (skip 4-byte header).
         * Read into the aligned DMA buffer, then copy to current_pkt
         * (which has the 2-byte offset for alignment). */
        unsigned int read_len = (pkt_len + 31) & ~31;  /* Align to 32 for DMA */
        if(read_len > RAW_RX_PKT_BUF_SIZE)
            read_len = RAW_RX_PKT_BUF_SIZE;

        bba_ins((cur_page << 8) | 4, rx_dma_buf, read_len);
        memcpy(raw_current_pkt + 2, rx_dma_buf, pkt_len);

        /* Advance RX read pointer to next packet */
        bba_out8(GCBBA_RRP,     (unsigned char)next_page);
        bba_out8(GCBBA_RRP + 1, 0x00);

        /* Process the packet */
        process_pkt(current_pkt);

        processed++;
    }

    /* Clear RX interrupt */
    bba_out8(GCBBA_IR, GCBBA_INT_RX);

    return processed;
}

/* ===== Main loop ===== */

void bba_loop(bool is_main_loop) {
    const target_ops_t *t = target_get_ops();
    uint64_t last_sec_tick = 0;
    unsigned int loop_secs_elapsed = 0;

    if(is_main_loop)
    {
        if(!(booted || running))
            disp_info();
    }

    if(timeout_loop > 0)
    {
        last_sec_tick = t->get_ticks();
    }

    while(!escape_loop)
    {
        /* Check for received packets */
        unsigned char ir = bba_in8(GCBBA_IR);

        if(ir & GCBBA_INT_RX)
            bba_rx();

        /* Handle RX buffer full */
        if(ir & GCBBA_INT_RX_FULL)
        {
            bba_out8(GCBBA_IR, GCBBA_INT_RX_FULL);
            bba_rx();
        }

        /* Handle errors */
        if(ir & (GCBBA_INT_RX_ERR | GCBBA_INT_TX_ERR |
                  GCBBA_INT_FIFO_ERR | GCBBA_INT_BUS_ERR))
        {
            bba_out8(GCBBA_IR,
                     ir & (GCBBA_INT_RX_ERR | GCBBA_INT_TX_ERR |
                           GCBBA_INT_FIFO_ERR | GCBBA_INT_BUS_ERR));
        }

        /* Poll for link change.  NWAYS bits 0/1 report 10/100 link-
         * status; either bit set means link is up.  Once RX is
         * enabled, these bits become sticky — mid-session cable
         * removal is NOT detected (accepted tradeoff). */
        if(__builtin_expect(!(bba_in8(GCBBA_NWAYS) & GCBBA_NWAYS_LINK_UP), 0))
        {
            screensaver_wake();

            if(booted && !running)
                disp_status("link change...");

            while(!(bba_in8(GCBBA_NWAYS) & GCBBA_NWAYS_LINK_UP))
                ;

            if(booted && !running)
                disp_status("idle...");

            /* If we were waiting in a DHCP timeout loop when link
             * changed, timeout immediately so we can retry. */
            if(timeout_loop > 0)
            {
                dhcp_attempts = 0;
                timeout_loop = -1;
                escape_loop = 1;
            }
        }

        if(is_main_loop)
        {
            dhcp_poll();
            screensaver_poll();
        }

        /* Timeout handling for DHCP */
        if(timeout_loop > 0)
        {
            uint64_t now = t->get_ticks();
            if((now - last_sec_tick) >= t->ticks_per_second) {
                last_sec_tick = now;
                loop_secs_elapsed++;
                if(dhcp_attempts > 1)
                {
                    disp_dhcp_attempts_count();
                    disp_dhcp_next_attempt(timeout_loop - loop_secs_elapsed + 1);
                }
                if(loop_secs_elapsed > (unsigned int)timeout_loop)
                {
                    timeout_loop = -1;
                    escape_loop = 1;
                }
            }
        }
    }
    escape_loop = 0;
}
