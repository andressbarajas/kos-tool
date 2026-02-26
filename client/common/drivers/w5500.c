/* client/common/drivers/w5500.c */
/*
 * WIZnet W5500 Ethernet controller driver for kos-tool.
 *
 * Operates in MACRAW mode (Socket 0) for raw Ethernet frame TX/RX.
 * SPI access is abstracted via w5500_spi_ops_t — each platform
 * (DC SCIF bit-bang, GC EXI) provides its own backend.
 *
 * Based on the KallistiOS W5500 network driver by SWAT (Ruslan Rostovtsev):
 *   https://github.com/KallistiOS/KallistiOS/pull/1261
 * Adapted for kos-tool's freestanding, polled-mode environment.
 *
 * Reference: WIZnet W5500 datasheet v1.1.0
 */

#include <string.h>
#include "w5500.h"
#include "adapter.h"
#include "packet.h"
#include "net.h"
#include "dcload.h"
#include "dhcp.h"
#include <kosload/target.h>
#include <kosload/screensaver.h>

/* ===== SPI backend (set by platform) ===== */

static const w5500_spi_ops_t *spi;

void w5500_set_spi_ops(const w5500_spi_ops_t *ops)
{
    spi = ops;
}

/* ===== Adapter instance ===== */

adapter_t adapter_w5500 = {
    "WIZnet W5500",
    { 0 },      /* MAC address (set in init) */
    { 0 },      /* 2-byte alignment pad */
    w5500_detect,
    w5500_init,
    w5500_start,
    w5500_stop,
    w5500_loop,
    w5500_tx
};

/* ===== Soft delay ===== */

static void delay_loop(volatile int count)
{
    while (count-- > 0)
        ;
}

/* ===== W5500 SPI I/O Functions =====
 *
 * W5500 SPI frame format (Variable Data Length Mode):
 *   [Address Phase: 16-bit addr MSB first]
 *   [Control Phase: BSB[4:0] | R/W | OM[1:0]]
 *   [Data Phase: 1+ bytes]
 *
 * BSB = Block Select Bits (selects common regs, socket regs, TX/RX buf)
 * R/W = 0 for read, 1 for write
 * OM  = 00 for Variable Data Length mode
 */

static unsigned char w5500_read_reg(unsigned char block, unsigned short addr)
{
    unsigned char cmd[3];
    unsigned char ret;

    cmd[0] = (addr >> 8) & 0xFF;
    cmd[1] = addr & 0xFF;
    cmd[2] = (block << 3) | W5500_SPI_READ | W5500_SPI_VDM;

    spi->cs_assert();
    spi->write_data(cmd, 3);
    ret = spi->read_byte();
    spi->cs_deassert();

    return ret;
}

static void w5500_write_reg(unsigned char block, unsigned short addr,
                            unsigned char data)
{
    unsigned char cmd[4];

    cmd[0] = (addr >> 8) & 0xFF;
    cmd[1] = addr & 0xFF;
    cmd[2] = (block << 3) | W5500_SPI_WRITE | W5500_SPI_VDM;
    cmd[3] = data;

    spi->cs_assert();
    spi->write_data(cmd, 4);
    spi->cs_deassert();
}

static unsigned short w5500_read_reg16(unsigned char block, unsigned short addr)
{
    unsigned char cmd[3];
    unsigned char data[2];

    cmd[0] = (addr >> 8) & 0xFF;
    cmd[1] = addr & 0xFF;
    cmd[2] = (block << 3) | W5500_SPI_READ | W5500_SPI_VDM;

    spi->cs_assert();
    spi->write_data(cmd, 3);
    spi->read_data(data, 2);
    spi->cs_deassert();

    return ((unsigned short)data[0] << 8) | data[1];
}

static void w5500_write_reg16(unsigned char block, unsigned short addr,
                              unsigned short data)
{
    unsigned char cmd[5];

    cmd[0] = (addr >> 8) & 0xFF;
    cmd[1] = addr & 0xFF;
    cmd[2] = (block << 3) | W5500_SPI_WRITE | W5500_SPI_VDM;
    cmd[3] = (data >> 8) & 0xFF;
    cmd[4] = data & 0xFF;

    spi->cs_assert();
    spi->write_data(cmd, 5);
    spi->cs_deassert();
}

static void w5500_read_buf(unsigned char block, unsigned short addr,
                           unsigned char *buf, unsigned short len)
{
    unsigned char cmd[3];

    cmd[0] = (addr >> 8) & 0xFF;
    cmd[1] = addr & 0xFF;
    cmd[2] = (block << 3) | W5500_SPI_READ | W5500_SPI_VDM;

    spi->cs_assert();
    spi->write_data(cmd, 3);
    spi->read_data(buf, len);
    spi->cs_deassert();
}

static void w5500_write_buf(unsigned char block, unsigned short addr,
                            const unsigned char *buf, unsigned short len)
{
    unsigned char cmd[3];

    cmd[0] = (addr >> 8) & 0xFF;
    cmd[1] = addr & 0xFF;
    cmd[2] = (block << 3) | W5500_SPI_WRITE | W5500_SPI_VDM;

    spi->cs_assert();
    spi->write_data(cmd, 3);
    spi->write_data(buf, len);
    spi->cs_deassert();
}

/*
 * Read a 16-bit register with consistency check.
 * The W5500 updates multi-byte registers non-atomically,
 * so we read twice and loop until we get matching values.
 */
static unsigned short w5500_read_reg16_safe(unsigned char block,
                                            unsigned short addr)
{
    unsigned short val, val1;

    do {
        val1 = w5500_read_reg16(block, addr);
        if (val1 != 0)
            val = w5500_read_reg16(block, addr);
        else
            val = 0;
    } while (val != val1);

    return val;
}

/*
 * Execute a socket command and wait for completion.
 * The W5500 clears the command register to 0x00 when done.
 * We keep CS asserted for the write+poll to minimize SPI overhead.
 */
static int w5500_exec_cmd(unsigned char block, unsigned char cmd)
{
    int i = 0;
    unsigned char cmd_buf[4];
    unsigned char status;

    /* Write the command */
    cmd_buf[0] = (W5500_Sn_CR >> 8) & 0xFF;
    cmd_buf[1] = W5500_Sn_CR & 0xFF;
    cmd_buf[2] = (block << 3) | W5500_SPI_WRITE | W5500_SPI_VDM;
    cmd_buf[3] = cmd;

    spi->cs_assert();
    spi->write_data(cmd_buf, 4);

    /* Poll until command register clears (within same CS assertion) */
    cmd_buf[0] = (W5500_Sn_CR >> 8) & 0xFF;
    cmd_buf[1] = W5500_Sn_CR & 0xFF;
    cmd_buf[2] = (block << 3) | W5500_SPI_READ | W5500_SPI_VDM;

    while (1) {
        spi->write_data(cmd_buf, 3);
        status = spi->read_byte();

        if (status == 0)
            break;

        if (i++ > 10000) {
            spi->cs_deassert();
            return -1;
        }
    }

    spi->cs_deassert();
    return 0;
}

/* ===== Soft Reset ===== */

static int w5500_soft_reset(void)
{
    int i = 0;

    w5500_write_reg(W5500_COMMON_BLOCK, W5500_MR, W5500_MR_RST);

    /* Wait for reset to complete (MR bit self-clears) */
    while (w5500_read_reg(W5500_COMMON_BLOCK, W5500_MR) & W5500_MR_RST) {
        if (i++ > 100)
            return -1;
        delay_loop(10000);
    }

    return 0;
}

/* ===== Link Status ===== */

static int w5500_link_up(void)
{
    return w5500_read_reg(W5500_COMMON_BLOCK, W5500_PHYCFGR) & 0x01;
}

/* ===== Detection ===== */

int w5500_detect(void)
{
    unsigned char ver;

    if (!spi)
        return -1;

    /* Initialize SPI */
    if (spi->init() < 0)
        return -1;

    /* Soft reset */
    if (w5500_soft_reset() < 0) {
        spi->shutdown();
        return -1;
    }

    /* Check chip version register — W5500 always reads 0x04 */
    ver = w5500_read_reg(W5500_COMMON_BLOCK, W5500_VERSIONR);
    spi->shutdown();

    if (ver != W5500_VERSION_EXPECTED)
        return -1;

    global_bg_color = W5500_BG_COLOR;
    installed_adapter = W5500_MODEL;

    return 0;
}

/* ===== Initialization ===== */

int w5500_init(void)
{
    int i;
    unsigned char ver;

    /* Initialize SPI */
    if (spi->init() < 0)
        return -1;

    /* Soft reset */
    if (w5500_soft_reset() < 0) {
        spi->shutdown();
        return -1;
    }

    /* PHY reset + auto-negotiate */
    w5500_write_reg(W5500_COMMON_BLOCK, W5500_PHYCFGR, 0x00);
    delay_loop(50000);
    w5500_write_reg(W5500_COMMON_BLOCK, W5500_PHYCFGR, 0xB8);
    delay_loop(50000);

    /* Verify chip version */
    ver = w5500_read_reg(W5500_COMMON_BLOCK, W5500_VERSIONR);
    if (ver != W5500_VERSION_EXPECTED) {
        spi->shutdown();
        return -1;
    }

    /* Disable all socket interrupts */
    w5500_write_reg(W5500_S0_REG_BLOCK, W5500_Sn_IMR, 0x00);

    /* Clear all socket buffer sizes (sockets 0-7) */
    for (i = 0; i < 8; i++) {
        unsigned char block = (unsigned char)(1 + 4 * i);
        w5500_write_reg(block, W5500_Sn_RXBUF_SIZE, 0);
        w5500_write_reg(block, W5500_Sn_TXBUF_SIZE, 0);
    }

    /* Assign all 16KB each to Socket 0 */
    w5500_write_reg(W5500_S0_REG_BLOCK, W5500_Sn_RXBUF_SIZE, 16);
    w5500_write_reg(W5500_S0_REG_BLOCK, W5500_Sn_TXBUF_SIZE, 16);

    /* Set MAC address */
    w5500_write_buf(W5500_COMMON_BLOCK, W5500_SHAR,
                    adapter_w5500.mac, 6);

    /* Open Socket 0 in MACRAW mode with MAC filter enabled */
    w5500_write_reg(W5500_S0_REG_BLOCK, W5500_Sn_MR,
                    W5500_Sn_MR_MACRAW | W5500_Sn_MR_MFEN);

    if (w5500_exec_cmd(W5500_S0_REG_BLOCK, W5500_CR_OPEN) < 0) {
        spi->shutdown();
        return -1;
    }

    /* Verify socket opened in MACRAW mode */
    if (w5500_read_reg(W5500_S0_REG_BLOCK, W5500_Sn_SR) != W5500_SOCK_MACRAW) {
        spi->shutdown();
        return -1;
    }

    return 0;
}

/* ===== Start / Stop ===== */

void w5500_start(void)
{
    /* Nothing additional needed — socket is already open from init */
}

void w5500_stop(void)
{
    /* Close Socket 0 and reset the chip */
    w5500_exec_cmd(W5500_S0_REG_BLOCK, W5500_CR_CLOSE);
    w5500_write_reg(W5500_COMMON_BLOCK, W5500_MR, W5500_MR_RST);
    spi->shutdown();
}

/* ===== Transmit ===== */

int w5500_tx(unsigned char *pkt, int len)
{
    unsigned short fsr, wr_ptr;

    /* Check link */
    if (!w5500_link_up())
        return 0;

    /* Check TX free space */
    fsr = w5500_read_reg16_safe(W5500_S0_REG_BLOCK, W5500_Sn_TX_FSR);
    if (fsr < (unsigned short)len)
        return 0;

    /* Write packet data to TX buffer at current write pointer */
    wr_ptr = w5500_read_reg16(W5500_S0_REG_BLOCK, W5500_Sn_TX_WR);
    w5500_write_buf(W5500_S0_TX_BLOCK, wr_ptr, (const unsigned char *)pkt, len);

    /* Advance write pointer */
    wr_ptr += len;
    w5500_write_reg16(W5500_S0_REG_BLOCK, W5500_Sn_TX_WR, wr_ptr);

    /* Issue SEND command */
    if (w5500_exec_cmd(W5500_S0_REG_BLOCK, W5500_CR_SEND) < 0)
        return 0;

    return 1;
}

/* ===== Receive ===== */

static int w5500_rx(void)
{
    unsigned short rsr, rd_ptr, data_len, read_len;
    unsigned char head[2];
    int processed = 0;

    /* Check if any data is available */
    rsr = w5500_read_reg16_safe(W5500_S0_REG_BLOCK, W5500_Sn_RX_RSR);

    if (rsr == 0)
        return 0;

    /* Read the current RX read pointer */
    rd_ptr = w5500_read_reg16(W5500_S0_REG_BLOCK, W5500_Sn_RX_RD);

    /* Read 2-byte MACRAW header (big-endian packet length including
     * the 2-byte header itself) */
    w5500_read_buf(W5500_S0_RX_BLOCK, rd_ptr, head, 2);
    rd_ptr += 2;

    data_len = ((unsigned short)head[0] << 8) | head[1];

    if (data_len < 2) {
        /* Invalid — skip this entry */
        rd_ptr += data_len;
        w5500_write_reg16(W5500_S0_REG_BLOCK, W5500_Sn_RX_RD, rd_ptr);
        w5500_exec_cmd(W5500_S0_REG_BLOCK, W5500_CR_RECV);
        return 0;
    }

    /* Actual payload length (header included its own 2 bytes) */
    data_len -= 2;

    /* Clamp to buffer size */
    read_len = data_len;
    if (read_len > RX_PKT_BUF_SIZE)
        read_len = RX_PKT_BUF_SIZE;

    /* Read packet into shared receive buffer (offset by 2 for alignment) */
    w5500_read_buf(W5500_S0_RX_BLOCK, rd_ptr, raw_current_pkt + 2, read_len);

    /* Advance read pointer past full packet */
    rd_ptr += data_len;
    w5500_write_reg16(W5500_S0_REG_BLOCK, W5500_Sn_RX_RD, rd_ptr);

    /* Tell W5500 we've consumed this data */
    w5500_exec_cmd(W5500_S0_REG_BLOCK, W5500_CR_RECV);

    /* Process the received Ethernet frame */
    process_pkt(current_pkt);
    processed = 1;

    return processed;
}

/* ===== Main Loop ===== */

void w5500_loop(int is_main_loop)
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
        w5500_rx();

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
