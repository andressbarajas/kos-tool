/* client/gamecube/net/enc28j60.c */
/*
 * Microchip ENC28J60 Ethernet controller driver for GameCube.
 *
 * The ENC28J60 is a standalone 10Base-T Ethernet controller accessed
 * via SPI-over-EXI. Used by GCNet (memory card slot) and ETH2GC
 * (Serial Port 2) adapters.
 *
 * SPI command format: 1-byte command (3-bit opcode + 5-bit address),
 * followed by data bytes. MAC/MII registers require a dummy byte
 * after the command before valid data is returned.
 *
 * The chip has 4 register banks selected via ECON1[1:0], plus 5
 * common registers (0x1B-0x1F) accessible from any bank.
 *
 * References:
 *   - Microchip DS39662E (datasheet)
 *   - Microchip DS80349C (silicon errata, all revisions B1-B7)
 *   - swiss-gc / libogc2 ENC28J60 drivers
 */

#include <string.h>
#include "enc28j60.h"
#include "packet.h"
#include "net.h"
#include "dcload.h"
#include "dhcp.h"
#include <kosload/target.h>
#include "../exi.h"
#include "../cache.h"
#include <kosload/screensaver.h>

adapter_t adapter_enc28j60 = {
    "ENC28J60 (GCNet/ETH2GC)",
    { 0 },      /* MAC address (generated in init) */
    { 0 },      /* 2-byte alignment pad */
    enc28j60_detect,
    enc28j60_init,
    enc28j60_start,
    enc28j60_stop,
    enc28j60_loop,
    enc28j60_tx
};

/* Runtime EXI location (set by detection) */
static int enc_channel;
static int enc_device;

/* Current register bank */
static unsigned char current_bank;

/* Next packet pointer for RX ring buffer */
static unsigned short next_pkt_ptr;

/* ===== Low-level EXI/SPI access ===== */

static int enc_exi_speed(void)
{
    /*
     * The GameCube EXI bus can run memory-card slots at 32MHz, but the
     * ENC28J60 SPI interface is rated for 20MHz max. Stay at 16MHz for
     * every ENJ placement so Slot A/B GCNet adapters are not overclocked.
     */
    return EXI_CLK_16MHZ;
}

static void enc_select(void)
{
    exi_select(enc_channel, enc_device, enc_exi_speed());
}

static void enc_deselect(void)
{
    exi_deselect(enc_channel);
}

/*
 * Select the register bank for the given software register address.
 * Common registers (0x1B-0x1F) don't need bank switching.
 */
static void enc_set_bank(unsigned char reg)
{
    unsigned char addr = ENC_ADDR(reg);

    /* Common registers are accessible from any bank */
    if (addr >= 0x1B)
        return;

    unsigned char bank = ENC_BANK(reg);
    if (bank == current_bank)
        return;

    /* Clear bank bits, then set new bank */
    enc_select();
    exi_imm(enc_channel, ENC28J60_CMD_BFC(ECON1) | ((ECON1_BSEL0 | ECON1_BSEL1) << 16), 2, EXI_IMM_WRITE);
    enc_deselect();

    if (bank) {
        enc_select();
        exi_imm(enc_channel, ENC28J60_CMD_BFS(ECON1) | ((unsigned int)bank << 16), 2, EXI_IMM_WRITE);
        enc_deselect();
    }

    current_bank = bank;
}

/*
 * Read an 8-bit control register.
 * MAC/MII registers (bit 7 set in addr) require a dummy byte.
 */
static unsigned char enc_read_reg(unsigned char reg)
{
    unsigned int val;
    int read_len;

    enc_set_bank(reg);

    /* MAC/MII regs need cmd + dummy + data = 3 bytes total,
     * ETH regs need cmd + data = 2 bytes total */
    read_len = ENC_IS_MAC_MII(reg) ? 3 : 2;

    enc_select();
    val = exi_imm(enc_channel, ENC28J60_CMD_RCR(ENC_ADDR(reg)), read_len, EXI_IMM_READWRITE);
    enc_deselect();

    /* EXI left-aligns received data in the 32-bit IMM register:
     *   2-byte: [garbage][data][0][0] → data at bits [23:16]
     *   3-byte: [garbage][dummy][data][0] → data at bits [15:8] */
    if (read_len == 3)
        return (val >> 8) & 0xFF;
    else
        return (val >> 16) & 0xFF;
}

/*
 * Write an 8-bit control register.
 */
static void enc_write_reg(unsigned char reg, unsigned char val)
{
    enc_set_bank(reg);

    enc_select();
    exi_imm(enc_channel, ENC28J60_CMD_WCR(ENC_ADDR(reg)) | ((unsigned int)val << 16), 2, EXI_IMM_WRITE);
    enc_deselect();
}

/*
 * Set bits in an ETH register (BFS command).
 * Only valid for ETH registers, not MAC/MII.
 */
static void enc_set_bits(unsigned char reg, unsigned char mask)
{
    enc_set_bank(reg);

    enc_select();
    exi_imm(enc_channel, ENC28J60_CMD_BFS(ENC_ADDR(reg)) | ((unsigned int)mask << 16), 2, EXI_IMM_WRITE);
    enc_deselect();
}

/*
 * Clear bits in an ETH register (BFC command).
 * Only valid for ETH registers, not MAC/MII.
 */
static void enc_clear_bits(unsigned char reg, unsigned char mask)
{
    enc_set_bank(reg);

    enc_select();
    exi_imm(enc_channel, ENC28J60_CMD_BFC(ENC_ADDR(reg)) | ((unsigned int)mask << 16), 2, EXI_IMM_WRITE);
    enc_deselect();
}

/*
 * Write a 16-bit value to a register pair (low, high).
 */
static void enc_write_reg16(unsigned char reg_l, unsigned short val)
{
    enc_write_reg(reg_l, val & 0xFF);
    enc_write_reg(reg_l + 1, val >> 8);
}

static void enc_delay_ms(unsigned int ms);
static void enc_delay_us(unsigned int us);

/* ===== PHY register access (indirect via MII interface) ===== */

static unsigned short enc_phy_read(unsigned char reg)
{
    unsigned short val;
    int timeout;

    enc_write_reg(MIREGADR, reg);
    enc_write_reg(MICMD, MICMD_MIIRD);
    enc_delay_us(15);

    /* Wait for MII read to complete (~10.24 us) */
    timeout = 10000;
    while (enc_read_reg(MISTAT) & MISTAT_BUSY) {
        if (--timeout == 0) {
            enc_write_reg(MICMD, 0x00);
            return 0xFFFF;
        }
        enc_delay_us(1);
        ;
    }

    enc_write_reg(MICMD, 0x00);
    enc_delay_us(1);

    val = enc_read_reg(MIRDL);
    val |= (unsigned short)enc_read_reg(MIRDH) << 8;
    return val;
}

static void enc_phy_write(unsigned char reg, unsigned short val)
{
    int timeout;

    enc_write_reg(MIREGADR, reg);
    enc_write_reg(MIWRL, val & 0xFF);
    enc_write_reg(MIWRH, val >> 8);  /* Writing high byte triggers MII write */
    enc_delay_us(15);

    /* Wait for MII write to complete */
    timeout = 10000;
    while (enc_read_reg(MISTAT) & MISTAT_BUSY) {
        if (--timeout == 0)
            break;
        enc_delay_us(1);
        ;
    }
}

static bool enc_phy_id_matches(unsigned short *phid1, unsigned short *phid2)
{
    int i;

    for (i = 0; i < 8; i++) {
        *phid1 = enc_phy_read(PHID1);
        *phid2 = enc_phy_read(PHID2);

        if (*phid1 == ENC28J60_PHID1_EXPECTED &&
            (*phid2 & 0xFC00) == (ENC28J60_PHID2_EXPECTED & 0xFC00))
            return true;

        enc_delay_ms(1);
    }

    return false;
}

/* ===== Buffer memory access ===== */

/*
 * Read from the RX buffer memory via RBM command.
 * Reads from the current ERDPT position (auto-increments).
 * Uses byte-by-byte immediate transfers for simplicity and
 * to avoid 32-byte alignment requirements of DMA.
 */
static void enc_read_buffer(unsigned char *buf, int len)
{
    int i;

    enc_select();
    /* Send RBM command byte */
    exi_imm(enc_channel, ENC28J60_CMD_RBM, 1, EXI_IMM_WRITE);

    /* Read data bytes */
    for (i = 0; i < len; i++) {
        unsigned int val = exi_imm(enc_channel, 0, 1, EXI_IMM_READ);
        buf[i] = (val >> 24) & 0xFF;
    }
    enc_deselect();
}

/*
 * Write to the TX buffer memory via WBM command.
 * Writes to the current EWRPT position (auto-increments).
 */
static void enc_write_buffer(const unsigned char *buf, int len)
{
    int i;

    enc_select();
    /* Send WBM command byte */
    exi_imm(enc_channel, ENC28J60_CMD_WBM, 1, EXI_IMM_WRITE);

    /* Write data bytes */
    for (i = 0; i < len; i++) {
        exi_imm(enc_channel, (unsigned int)buf[i] << 24, 1, EXI_IMM_WRITE);
    }
    enc_deselect();
}

/* ===== Soft delay ===== */

static void delay_loop(volatile int count)
{
    while (count-- > 0)
        ;
}

static void enc_delay_ms(unsigned int ms)
{
    const target_ops_t *t = target_get_ops();
    uint64_t start;
    uint64_t ticks;

    if (!t || !t->get_ticks || !t->ticks_per_second) {
        while (ms-- > 0)
            delay_loop(100000);
        return;
    }

    ticks = (uint64_t)(t->ticks_per_second / 1000) * ms;
    if (ticks == 0)
        ticks = 1;

    start = t->get_ticks();
    while ((t->get_ticks() - start) < ticks)
        ;
}

static void enc_delay_us(unsigned int us)
{
    const target_ops_t *t = target_get_ops();
    uint64_t start;
    uint64_t ticks;

    if (!t || !t->get_ticks || !t->ticks_per_second) {
        delay_loop((int)us * 100);
        return;
    }

    ticks = (uint64_t)(t->ticks_per_second / 1000000) * us;
    if (ticks == 0)
        ticks = 1;

    start = t->get_ticks();
    while ((t->get_ticks() - start) < ticks)
        ;
}

/*
 * Wake the ENC28J60 out of power-save before issuing SPI soft reset.
 *
 * Silicon errata #19 says the System Reset command is ignored while
 * ECON2.PWRSV is set. This shows up most often on GameCube when a loaded
 * program returns after using the adapter and leaves the controller in a
 * non-default low-power state.
 */
static void enc_wake_and_reset(int channel, int device)
{
    exi_select(channel, device, EXI_CLK_1MHZ);
    exi_imm(channel,
            ENC28J60_CMD_BFC(ECON2) | ((unsigned int)ECON2_PWRSV << 16),
            2, EXI_IMM_WRITE);
    exi_deselect(channel);

    /* Allow the regulator to stabilize before reset (errata #19). */
    enc_delay_ms(2);

    exi_select(channel, device, EXI_CLK_1MHZ);
    exi_imm(channel, ENC28J60_CMD_SRC, 1, EXI_IMM_WRITE);
    exi_deselect(channel);

    /* Errata #2: wait >= 1 ms after reset; don't poll CLKRDY.
     * Leave extra room for the PHY/MIIM path to become readable too. */
    enc_delay_ms(5);
}

static bool enc_wait_for_link_up(void)
{
    const target_ops_t *t = target_get_ops();
    uint64_t start = t->get_ticks();
    uint64_t timeout = t->ticks_per_second;

    do {
        if (enc_phy_read(PHSTAT2) & PHSTAT2_LSTAT)
            return true;

        enc_delay_ms(1);
    } while ((t->get_ticks() - start) < timeout);

    return false;
}

static void enc_reset_tx_logic(void)
{
    enc_clear_bits(ECON1, ECON1_TXRTS);
    enc_set_bits(ECON1, ECON1_TXRST);
    enc_clear_bits(ECON1, ECON1_TXRST);
    enc_clear_bits(EIR, EIR_TXIF | EIR_TXERIF);
}

static void enc_restore_runtime_config(void)
{
    if (enc_read_reg(ECON2) & ECON2_PWRSV) {
        enc_clear_bits(ECON2, ECON2_PWRSV);
        enc_delay_ms(2);
    }

    enc_set_bits(ECON2, ECON2_AUTOINC);
    enc_write_reg(ERXFCON, ERXFCON_UCEN | ERXFCON_BCEN | ERXFCON_CRCEN);
    enc_write_reg(MACON1, MACON1_MARXEN | MACON1_TXPAUS | MACON1_RXPAUS);
    enc_write_reg(MACON2, 0x00);
    enc_write_reg(MACON3, MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN);
    enc_write_reg(MACON4, MACON4_DEFER);
    enc_write_reg16(MAMXFLL, ENC_MAX_FRAME);
    enc_clear_bits(EIR,
                   EIR_RXERIF | EIR_TXERIF | EIR_TXIF |
                   EIR_LINKIF | EIR_DMAIF | EIR_PKTIF);
    enc_set_bank(ERDPTL);
}

static void enc_rearm_rx_ring(void)
{
    int packets;

    /* Reprogramming ERXST/ERXND while RX is disabled resets the internal
     * hardware write pointer to ERXST. That gives us a clean RX ring without
     * repeatedly using RXRST during normal return-to-menu rearming. */
    enc_write_reg16(ERXSTL, ENC_RX_START);
    enc_write_reg16(ERXNDL, ENC_RX_END);
    enc_write_reg16(ERXRDPTL, ENC_RX_END);
    enc_write_reg16(ERDPTL, ENC_RX_START);
    next_pkt_ptr = ENC_RX_START;

    packets = 255;
    while ((enc_read_reg(EPKTCNT) != 0) && packets-- > 0)
        enc_set_bits(ECON2, ECON2_PKTDEC);

    enc_clear_bits(EIR, EIR_RXERIF | EIR_PKTIF);
}

static int enc_rx_next_valid(unsigned short ptr)
{
    return ptr <= ENC_RX_END && ((ptr & 1) == 0);
}

static void enc_rearm_runtime(void)
{
    int timeout;

    enc_clear_bits(ECON1, ECON1_RXEN | ECON1_TXRTS);

    timeout = 10000;
    while ((enc_read_reg(ESTAT) & ESTAT_RXBUSY) && timeout-- > 0)
        enc_delay_us(1);

    enc_restore_runtime_config();
    enc_reset_tx_logic();
    enc_rearm_rx_ring();
    enc_restore_runtime_config();
    enc_set_bits(ECON1, ECON1_RXEN);
}

/* ===== MAC address generation =====
 *
 * The ENC28J60 has no factory-programmed MAC address.
 * Generate one deterministically from the GameCube's ECID
 * (Electronic Chip ID) hardware registers, matching libogc2's
 * approach. OUI = 00:09:BF (Nintendo).
 */

static inline unsigned int mfspr_ecid0(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39C" : "=r"(val));
    return val;
}

static inline unsigned int mfspr_ecid1(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39D" : "=r"(val));
    return val;
}

static inline unsigned int mfspr_ecid2(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39E" : "=r"(val));
    return val;
}

static inline unsigned int mfspr_ecid3(void)
{
    unsigned int val;
    __asm__ volatile("mfspr %0, 0x39F" : "=r"(val));
    return val;
}

static void enc_generate_mac(unsigned char *mac)
{
    union {
        unsigned int cid[4];
        unsigned char data[19];
    } ecid = { { 0 } };
    unsigned int sum;
    int i;

    ecid.cid[0] = mfspr_ecid0();
    ecid.cid[1] = mfspr_ecid1();
    ecid.cid[2] = mfspr_ecid2();
    ecid.cid[3] = mfspr_ecid3();

    /* Mix in a fixed salt (matches libogc2) */
    ecid.data[15] ^= 0x00;
    ecid.data[16] ^= 0x04;
    ecid.data[17] ^= 0xA3;

    /* Hash ECID with channel number as seed for uniqueness per slot */
    sum = (unsigned int)enc_channel;
    for (i = 0; i < 18; i += 3) {
        unsigned int word;
        word  = (unsigned int)ecid.data[i]     << 16;
        word |= (unsigned int)ecid.data[i + 1] << 8;
        word |= (unsigned int)ecid.data[i + 2];
        sum += word;
        sum = (sum & 0x00FFFFFF) + (sum >> 24);
    }

    /* OUI = 00:09:BF (Nintendo) */
    mac[0] = 0x00;
    mac[1] = 0x09;
    mac[2] = 0xBF;
    mac[3] = (sum >> 16) & 0xFF;
    mac[4] = (sum >> 8) & 0xFF;
    mac[5] = sum & 0xFF;
}

/* ===== Detection ===== */

/*
 * Probe a specific EXI location for an ENC28J60.
 *
 * The ENC28J60 returns 0xFA050000 from the standard EXI ID command,
 * but only when its ERDPT registers are at power-on defaults (0x05FA).
 * If swiss-gc or another loader previously initialized the chip, those
 * registers will have been modified and the ID won't match.
 *
 * Strategy: try the standard ID first (fast), then soft-reset and retry.
 */
static int enc_probe(int channel, int device)
{
    unsigned int id;

    /* Fast path: works on fresh power-on */
    id = exi_get_id(channel, device);
    if (id == ENC28J60_EXI_ID)
        goto found;

    /* Soft-reset potential ENC28J60 and retry. After swiss-gc uses the
     * chip, ERDPT registers are modified so exi_get_id() returns a
     * different value (could even be 0x00000000). Clear PWRSV first so
     * the reset still works if a prior program left the chip asleep.
     * Harmless to non-ENC28J60 devices (undefined SPI command). */
    enc_wake_and_reset(channel, device);

    /* Retry — registers should now be at power-on defaults */
    id = exi_get_id(channel, device);
    if (id == ENC28J60_EXI_ID)
        goto found;

    return 0;

found:
    enc_channel = channel;
    enc_device = device;
    return 1;
}

int enc28j60_detect(void)
{
    /* Probe all possible EXI locations where GCNet/ETH2GC can sit:
     *   Memory Card Slot A: Channel 0, Device 0
     *   Memory Card Slot B: Channel 1, Device 0
     *   Serial Port 1 (BBA slot): Channel 0, Device 2
     *   Serial Port 2: Channel 2, Device 0
     */
    if (enc_probe(0, 0) ||
        enc_probe(1, 0) ||
        enc_probe(0, 2) ||
        enc_probe(2, 0))
    {
        global_bg_color = ENC_BG_COLOR;
        installed_adapter = ENC_MODEL;
        return 0;
    }

    return -1;
}

/* ===== Initialization ===== */

int enc28j60_init(void)
{
    unsigned short phid1;
    unsigned short phid2;

    current_bank = 0;
    enc_wake_and_reset(enc_channel, enc_device);

    /* Validate PHY ID */
    if (!enc_phy_id_matches(&phid1, &phid2))
        return -1;

    /* --- RX buffer setup ---
     * Errata #5: ERXST must be 0x0000 */
    enc_write_reg16(ERXSTL, ENC_RX_START);
    enc_write_reg16(ERXNDL, ENC_RX_END);

    /* Initialize read pointers.
     * Errata #14: ERXRDPT must be odd. ENC_RX_END = 0x0FFF is odd. */
    enc_write_reg16(ERXRDPTL, ENC_RX_END);
    enc_write_reg16(ERDPTL, ENC_RX_START);
    next_pkt_ptr = ENC_RX_START;

    /* --- TX buffer setup --- */
    enc_write_reg16(ETXSTL, ENC_TX_START);
    enc_write_reg16(EWRPTL, ENC_TX_START);

    /* Keep buffer pointer auto-increment enabled after reset. */
    enc_set_bits(ECON2, ECON2_AUTOINC);

    /* --- Receive filter ---
     * Accept: unicast to our MAC + broadcast + CRC valid */
    enc_write_reg(ERXFCON, ERXFCON_UCEN | ERXFCON_BCEN | ERXFCON_CRCEN);

    /* --- MAC configuration --- */
    enc_write_reg(MACON1, MACON1_MARXEN | MACON1_TXPAUS | MACON1_RXPAUS);
    enc_write_reg(MACON2, 0x00);

    /* Pad short frames to 60 bytes + auto CRC + frame length check.
     * Half-duplex (10Base-T). */
    enc_write_reg(MACON3, MACON3_PADCFG0 | MACON3_TXCRCEN | MACON3_FRMLNEN);

    /* Defer to wire activity (required for half-duplex) */
    enc_write_reg(MACON4, MACON4_DEFER);

    /* Max frame length */
    enc_write_reg16(MAMXFLL, ENC_MAX_FRAME);

    /* Inter-packet gap timing (half-duplex values) */
    enc_write_reg(MABBIPG, 0x12);
    enc_write_reg(MAIPGL, 0x12);
    enc_write_reg(MAIPGH, 0x0C);

    /* --- MAC address --- */
    enc_generate_mac(adapter_enc28j60.mac);

    /* ENC28J60 MAC register order: MAADR1=first byte on wire
     * Register layout at bank 3: MAADR5, MAADR6, MAADR3, MAADR4, MAADR1, MAADR2 */
    enc_write_reg(MAADR1, adapter_enc28j60.mac[0]);
    enc_write_reg(MAADR2, adapter_enc28j60.mac[1]);
    enc_write_reg(MAADR3, adapter_enc28j60.mac[2]);
    enc_write_reg(MAADR4, adapter_enc28j60.mac[3]);
    enc_write_reg(MAADR5, adapter_enc28j60.mac[4]);
    enc_write_reg(MAADR6, adapter_enc28j60.mac[5]);

    /* --- PHY configuration --- */

    /* Clear any prior power-save / loopback state the loaded program may
     * have left behind before we re-enable the link. */
    enc_phy_write(PHCON1, 0x0000);

    /* Errata #9: disable half-duplex loopback */
    enc_phy_write(PHCON2, PHCON2_HDLDIS);

    /* LED configuration: LA=link, LB=activity, stretch */
    enc_phy_write(PHLCON, 0x3C12);

    /* Enable link change interrupt on PHY */
    enc_phy_write(PHIE, PHIE_PLNKIE | PHIE_PGEIE);

    /* Clear any sticky status before going live again. */
    enc_clear_bits(EIR,
                   EIR_RXERIF | EIR_TXERIF | EIR_TXIF |
                   EIR_LINKIF | EIR_DMAIF | EIR_PKTIF);

    /* Disable CLKOUT pin (not needed) */
    enc_write_reg(ECOCON, 0x00);

    /* Give the PHY up to ~1s to relink so the next host upload doesn't
     * race a just-reset adapter. */
    if (enc_wait_for_link_up())
        enc_delay_ms(50);

    /* Return to bank 0 for runtime operation */
    enc_set_bank(ERDPTL);

    return 0;
}

void enc28j60_start(void)
{
    enc_rearm_runtime();
}

void enc28j60_stop(void)
{
    int timeout;

    /* Disable receive and cancel any in-flight transmit request. */
    enc_clear_bits(ECON1, ECON1_RXEN | ECON1_TXRTS);

    timeout = 10000;
    while ((enc_read_reg(ESTAT) & ESTAT_RXBUSY) && timeout-- > 0)
        enc_delay_us(1);
}

/* ===== TX ===== */

int enc28j60_tx(unsigned char *pkt, int len)
{
    int attempt;

    /* Pad to minimum Ethernet frame size (60 bytes without CRC).
     * Zero-pad to prevent leaking prior packet data (EtherLeak). */
    if (len < 60) {
        memset(pkt + len, 0, 60 - len);
        len = 60;
    }

    for (attempt = 0; attempt < 4; attempt++) {
        unsigned char eir = 0;
        unsigned char estat;
        int timeout;

        /* Errata #12: reset TX logic before each transmission */
        enc_reset_tx_logic();

        /* Set write pointer to TX buffer start */
        enc_write_reg16(EWRPTL, ENC_TX_START);

        /* Set TX start pointer */
        enc_write_reg16(ETXSTL, ENC_TX_START);

        /* Set TX end pointer (start + 1 control byte + packet data - 1) */
        enc_write_reg16(ETXNDL, ENC_TX_START + len);

        /* Write per-packet control byte (0x00 = use MACON3 defaults) */
        unsigned char control = 0x00;
        enc_write_buffer(&control, 1);

        /* Write packet data */
        enc_write_buffer(pkt, len);

        /* Start transmission */
        enc_set_bits(ECON1, ECON1_TXRTS);

        /* Wait for transmit complete */
        timeout = 20000;
        while (timeout > 0) {
            eir = enc_read_reg(EIR);
            if (eir & (EIR_TXIF | EIR_TXERIF))
                break;
            enc_delay_us(1);
            timeout--;
        }

        estat = enc_read_reg(ESTAT);

        if (timeout != 0 && !(eir & EIR_TXERIF) && !(estat & ESTAT_TXABRT)) {
            enc_clear_bits(ECON1, ECON1_TXRTS);
            enc_clear_bits(EIR, EIR_TXIF | EIR_TXERIF);
            return 1;
        }

        enc_reset_tx_logic();
        enc_delay_us(100);
    }

    return 0;
}

/* ===== RX ===== */

static int enc28j60_rx(void)
{
    int processed = 0;
    unsigned char rsv[6];
    unsigned short pkt_next, pkt_len, pkt_status;
    unsigned short erxrdpt;

    /* Errata #6: use EPKTCNT instead of EIR.PKTIF */
    while (enc_read_reg(EPKTCNT) > 0) {
        /* Set read pointer to next packet position */
        enc_write_reg16(ERDPTL, next_pkt_ptr);

        /* Read 6-byte Receive Status Vector via RBM */
        enc_read_buffer(rsv, 6);

        pkt_next   = (unsigned short)rsv[0] | ((unsigned short)rsv[1] << 8);
        pkt_len    = (unsigned short)rsv[2] | ((unsigned short)rsv[3] << 8);
        pkt_status = (unsigned short)rsv[4] | ((unsigned short)rsv[5] << 8);

        if (!enc_rx_next_valid(pkt_next) || pkt_len > (RX_PKT_BUF_SIZE + 4)) {
            enc_rearm_runtime();
            break;
        }

        /* Validate: check "Received OK" bit and reasonable length */
        if ((pkt_status & 0x0080) && pkt_len >= 18 && pkt_len <= (RX_PKT_BUF_SIZE + 4)) {
            /* Subtract 4-byte CRC for actual payload */
            unsigned short payload_len = pkt_len - 4;

            /* Read packet data directly into current_pkt buffer */
            enc_read_buffer(raw_current_pkt + 2, payload_len);

            /* Process the packet */
            process_pkt(current_pkt);
            processed++;
        }

        /* Advance next packet pointer */
        next_pkt_ptr = pkt_next;

        /* Update hardware read pointer.
         * Errata #14: ERXRDPT must be odd.
         * next_pkt_ptr is always even, so next_pkt_ptr - 1 is odd.
         * Handle wraparound at buffer start. */
        if (pkt_next == ENC_RX_START)
            erxrdpt = ENC_RX_END;
        else
            erxrdpt = pkt_next - 1;

        enc_write_reg16(ERXRDPTL, erxrdpt);

        /* Decrement hardware packet counter */
        enc_set_bits(ECON2, ECON2_PKTDEC);
    }

    return processed;
}

/* ===== Main loop ===== */

void enc28j60_loop(bool is_main_loop)
{
    const target_ops_t *t = target_get_ops();
    uint64_t last_sec_tick = 0;
    unsigned int loop_secs_elapsed = 0;

    if (is_main_loop) {
        if (!(booted || running))
            disp_info();
    }

    if (timeout_loop > 0) {
        last_sec_tick = t->get_ticks();
    }

    while (!escape_loop) {
        /* Poll for received packets */
        enc28j60_rx();

        if (is_main_loop) {
            dhcp_poll();
            screensaver_poll();
        }

        /* Timeout handling for DHCP */
        if (timeout_loop > 0) {
            uint64_t now = t->get_ticks();
            if ((now - last_sec_tick) >= t->ticks_per_second) {
                last_sec_tick = now;
                loop_secs_elapsed++;
                if (dhcp_attempts > 1) {
                    disp_dhcp_attempts_count();
                    disp_dhcp_next_attempt(timeout_loop - loop_secs_elapsed + 1);
                }
                if (loop_secs_elapsed > (unsigned int)timeout_loop) {
                    timeout_loop = -1;
                    escape_loop = 1;
                }
            }
        }
    }
    escape_loop = 0;
}
