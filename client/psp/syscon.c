/* client/psp/syscon.c — PSP syscon SSP driver (bare-metal, no firmware syscall).
 *
 * ============================ PROVENANCE — READ ME ============================
 * Drives the syscon serial port (0xBE580000, an ARM PrimeCell SSP) to send
 * command packets to the SC microcontroller.  The SSP register map, the packet
 * format ([cmd, len, params..., hash]), the hash formula (~sum & 0xFF), and the
 * command bytes below were recovered CLEAN-ROOM by disassembling Sony's
 * decrypted 6.61 sceSyscon_driver (syscon.prx); the code is ours.  Register
 * meanings cross-checked against the psdevwiki hardware docs (0xBE580000).
 *
 * HONEST GAP (untested on hardware):
 *   - the LED-control command is byte 0x47 with a per-LED bit (0x10 = the
 *     display/backlight rail); WHICH bit is the backlight is INFERRED from the
 *     driver's LED-id->bit table (the binary is name-stripped), so treat the
 *     backlight identification as unverified.
 * =============================================================================
 */

#include <stdint.h>
#include <stdbool.h>
#include "syscon.h"

#define SYSCON_BASE   0xBE580000u
#define SC(off)       (*(volatile uint32_t *)(SYSCON_BASE + (uint32_t)(off)))
#define SC_CR0        0x00u  /* serial format / clock divider; init = 0xCF */
#define SC_CR1        0x04u  /* 4 = disabled, 6 = enabled                 */
#define SC_DATA       0x08u  /* 16-bit data FIFO                          */
#define SC_STATUS     0x0Cu  /* standard ARM PL022 SSPSR                  */
#define SC_IMSC       0x14u  /* interrupt mask                            */
#define SC_RIS        0x18u  /* raw interrupt status                      */
#define SC_ICR        0x20u  /* interrupt clear                           */
#define SC_DMACR      0x24u  /* DMA control                               */

#define SC_SR_TFE     0x01u  /* transmit FIFO empty                       */
#define SC_SR_RNE     0x04u  /* receive FIFO not empty                    */
#define SC_RIS_ROR    0x01u  /* receive overrun                           */

/* GPIO, RE'd from lowio.prx's sceGpio_driver exports:
 *   NID 0x310f0ccf @ 0x3214 -> sw a0, 0xBE240008 ; sync   (set)
 *   NID 0x103c3eb2 @ 0x322c -> sw a0, 0xBE24000C ; sync   (clear)
 * syscon.prx's transmit path clears pin mask 0x08 before the packet and sets it
 * afterwards; the transaction is not accepted without it. */
#define GPIO_MODE0        (*(volatile uint32_t *)0xBE240000u)
#define GPIO_PORT         (*(volatile uint32_t *)0xBE240004u)
#define GPIO_SET          (*(volatile uint32_t *)0xBE240008u)
#define GPIO_CLEAR        (*(volatile uint32_t *)0xBE24000Cu)
#define GPIO_INTR_MODE0   (*(volatile uint32_t *)0xBE240010u)
#define GPIO_INTR_MODE1   (*(volatile uint32_t *)0xBE240014u)
#define GPIO_INTR_MODE2   (*(volatile uint32_t *)0xBE240018u)
#define GPIO_INTR_ENABLE  (*(volatile uint32_t *)0xBE24001Cu)
#define GPIO_INTR_PENDING (*(volatile uint32_t *)0xBE240020u)
#define GPIO_INTR_ACK     (*(volatile uint32_t *)0xBE240024u)
#define GPIO_MODE1        (*(volatile uint32_t *)0xBE240040u)
#define GPIO_TX_PIN       0x08u /* GPIO3: request / transaction gate */
#define GPIO_RX_PIN       0x10u /* GPIO4: response-complete signal   */

/* SSP clock and IO enables, RE'd from Sony's syscon init (syscon.prx text+0x2b0)
 * and independently from the bare-metal 6.61 IPL at 0x04005d08.  All three
 * calls and their arguments are now accounted for:
 *
 *   sceSysregSpiClkSelect(0, 1): BC100064 bits 2:0 = 1
 *   sceSysregSpiClkEnable(0):    BC100058 bit 0 = 1
 *   sceSysregSpiIoEnable(0):     BC100078 bit 24 = 1
 *
 * The GPIO clock/IO bits below make the path independent of inherited XMB
 * state.  No sysreg reset is used: neither retail init contains one. */
#define SYSREG_CLK_ENABLE  (*(volatile uint32_t *)0xBC100058u)
#define SYSREG_CLK_SELECT  (*(volatile uint32_t *)0xBC100064u)
#define SYSREG_SPI_IO      (*(volatile uint32_t *)0xBC100078u)
#define SYSREG_GPIO_IO     (*(volatile uint32_t *)0xBC10007Cu)
#define SYSREG_SPI_CLK_BIT (1u << 0)
#define SYSREG_GPIO_CLK_BIT (1u << 23)
#define SYSREG_SPI_IO_BIT  (1u << 24)

/* Preserve the existing diagnostic result numbers. */
#define SC_ERR_DRAIN     1   /* FIFO never drained before the packet */
#define SC_ERR_COMPLETE  2   /* GPIO4 never reported completion       */
#define SC_ERR_STATUS    3   /* controller or response packet error   */
#define SC_RESULT_RETRY  4   /* internal: send Sony's command-only poll */

#define SC_FIFO_WORDS      8  /* PL022 FIFO and Sony response limit: 16 bytes */
#define SC_POLL_COUNT_TICKS 0x01000000u /* comfortably sub-second on Allegrex */
#define SC_ACK_COUNT_TICKS  0x00100000u /* short stale-GPIO4 clear deadline    */
#define SC_SETTLE_COUNT_TICKS 0x00080000u /* about 4 ms before control commands */
#define SC_RETRY_COUNT_TICKS  0x00280000u /* 5x settle: about 20 ms per poll    */
#define SC_RETRY_LIMIT        5            /* bounded to roughly 100 ms total    */

static bool inited;

/* Most recent checksum-valid reply; byte 0 is the status byte (see syscon.h).
 * sc_reply_seq separates "this call produced a reply" from "an older one is
 * still on file". */
static uint8_t  sc_reply[16];
static int      sc_reply_len;
static uint32_t sc_reply_seq;

static inline void sc_sync(void) { __asm__ volatile("sync" ::: "memory"); }
static inline uint32_t sc_count(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $9" : "=r"(value));
    return value;
}

static void sc_delay(uint32_t ticks) {
    uint32_t start = sc_count();

    while((uint32_t)(sc_count() - start) < ticks)
        ;
}

__attribute__((weak, noinline))
void psp_syscon_trace(unsigned int event, uint32_t value) {
    (void)event;
    (void)value;
}

void psp_syscon_init(void) {
    uint32_t gpio;

    SYSREG_CLK_SELECT = (SYSREG_CLK_SELECT & ~7u) | 1u;
    sc_sync();
    psp_syscon_trace(PSP_SYSCON_TRACE_CLK_SELECT, SYSREG_CLK_SELECT);

    SYSREG_CLK_ENABLE |= SYSREG_SPI_CLK_BIT | SYSREG_GPIO_CLK_BIT;
    sc_sync();
    psp_syscon_trace(PSP_SYSCON_TRACE_CLK_ENABLE, SYSREG_CLK_ENABLE);

    SYSREG_SPI_IO |= SYSREG_SPI_IO_BIT;
    sc_sync();
    psp_syscon_trace(PSP_SYSCON_TRACE_IO_ENABLE, SYSREG_SPI_IO);

    SC(SC_CR0) = 0xCF;
    SC(SC_CR1) = 4;
    SC(SC_IMSC) = 0;
    SC(SC_DMACR) = 0;

    /* IPL GPIO setup: GPIO3 mode 0/output, GPIO4 mode 1/input, GPIO4
     * interrupt mode 3 and interrupt detection enabled. */
    GPIO_CLEAR = GPIO_TX_PIN;
    gpio = GPIO_MODE0;
    GPIO_MODE0 = (gpio | GPIO_TX_PIN) & ~GPIO_RX_PIN;
    gpio = GPIO_MODE1;
    GPIO_MODE1 = (gpio & ~GPIO_TX_PIN) | GPIO_RX_PIN;
    GPIO_INTR_MODE0 &= ~GPIO_RX_PIN;
    GPIO_INTR_MODE1 &= ~GPIO_RX_PIN;
    GPIO_INTR_MODE2 |= GPIO_RX_PIN;
    GPIO_INTR_ENABLE |= GPIO_RX_PIN;

    /* Program the output latch and pin modes before exposing the pins.  This
     * prevents a high GPIO3 glitch from looking like a transaction request if
     * GPIO IO was genuinely disabled on entry. */
    SYSREG_GPIO_IO |= GPIO_TX_PIN | GPIO_RX_PIN;
    GPIO_INTR_ACK = GPIO_RX_PIN;
    sc_sync();

    psp_syscon_trace(PSP_SYSCON_TRACE_CR0, SC(SC_CR0));
    psp_syscon_trace(PSP_SYSCON_TRACE_CR1, SC(SC_CR1));
    psp_syscon_trace(PSP_SYSCON_TRACE_SR_INITIAL, SC(SC_STATUS));
    inited = true;
}

static int sc_exchange(const uint8_t *tx, int total, uint32_t settle_ticks) {
    uint8_t rx[16];
    uint32_t start, status, word, pending;
    unsigned int sum;
    int i, rx_count, rlen, result, controller_error;

    result = SC_ERR_STATUS;
    controller_error = 0;
    rx_count = 0;
    for(i = 0; i < 16; i++)
        rx[i] = 0xFF;

    /* Match the IPL start path: sample the port, force GPIO3 low, discard a
     * stale GPIO4 completion, and drain at most the physical FIFO depth. */
    (void)GPIO_PORT;
    GPIO_CLEAR = GPIO_TX_PIN;
    sc_sync();

    /* Retail waits before both the initial control packet and a command-only
     * second phase.  Keep GPIO3 low during that interval, then perform all
     * stale-state checks immediately before loading the transaction. */
    if(settle_ticks)
        sc_delay(settle_ticks);

    GPIO_INTR_ACK = GPIO_RX_PIN;
    sc_sync();

    /* Do not mistake a firmware transaction's stale completion for ours.
     * GPIO3 is already low, so a pending GPIO4 edge must clear before TX. */
    start = sc_count();
    do {
        pending = GPIO_INTR_PENDING;
        if(!(pending & GPIO_RX_PIN))
            break;
        GPIO_INTR_ACK = GPIO_RX_PIN;
        sc_sync();
    } while((uint32_t)(sc_count() - start) < SC_ACK_COUNT_TICKS);
    if(pending & GPIO_RX_PIN) {
        psp_syscon_trace(PSP_SYSCON_TRACE_GPIO4, pending);
        goto cleanup;
    }

    status = SC(SC_STATUS);
    psp_syscon_trace(PSP_SYSCON_TRACE_DRAIN, status);
    for(i = 0; i < SC_FIFO_WORDS && (status & SC_SR_RNE); i++) {
        (void)SC(SC_DATA);
        status = SC(SC_STATUS);
        psp_syscon_trace(PSP_SYSCON_TRACE_DRAIN, status);
    }
    if(status & SC_SR_RNE) {
        result = SC_ERR_DRAIN;
        goto cleanup;
    }

    SC(SC_ICR) = 3;
    for(i = 0; i < total; i += 2) {
        (void)SC(SC_STATUS);
        SC(SC_DATA) = ((uint32_t)tx[i] << 8) | tx[i + 1];
    }
    SC(SC_CR1) = 6;
    GPIO_SET = GPIO_TX_PIN;
    sc_sync();
    psp_syscon_trace(PSP_SYSCON_TRACE_TX, SC(SC_STATUS));

    /* Sony's synchronous IPL executor polls the GPIO4 interrupt-pending bit
     * and calls the packet-end handler directly.  CPU interrupt delivery is
     * not involved, so this works with Status.IE clear. */
    start = sc_count();
    do {
        pending = GPIO_INTR_PENDING;
        if(pending & GPIO_RX_PIN)
            break;
    } while((uint32_t)(sc_count() - start) < SC_POLL_COUNT_TICKS);
    psp_syscon_trace(PSP_SYSCON_TRACE_GPIO4, pending);
    if(!(pending & GPIO_RX_PIN)) {
        result = SC_ERR_COMPLETE;
        goto cleanup;
    }
    GPIO_INTR_ACK = GPIO_RX_PIN;
    /* Retail drops the request gate immediately on entering its GPIO4 packet-
     * end handler, before it examines or drains the response FIFO. */
    GPIO_CLEAR = GPIO_TX_PIN;
    sc_sync();

    status = SC(SC_STATUS);
    psp_syscon_trace(PSP_SYSCON_TRACE_RX, status);
    if(!(status & SC_SR_RNE) || !(status & SC_SR_TFE))
        controller_error = 1;
    if(SC(SC_RIS) & SC_RIS_ROR) {
        SC(SC_ICR) = SC_RIS_ROR;
        controller_error = 1;
    }

    for(i = 0; i < SC_FIFO_WORDS && (SC(SC_STATUS) & SC_SR_RNE); i++) {
        word = SC(SC_DATA);
        rx[rx_count++] = (uint8_t)(word >> 8);
        rx[rx_count++] = (uint8_t)word;
    }
    status = SC(SC_STATUS);
    psp_syscon_trace(PSP_SYSCON_TRACE_RX, status);
    if((status & SC_SR_RNE) || controller_error)
        goto cleanup;

    if(rx_count >= 4) {
        uint32_t first = ((uint32_t)rx[0] << 24) |
                         ((uint32_t)rx[1] << 16) |
                         ((uint32_t)rx[2] << 8) | rx[3];
        psp_syscon_trace(PSP_SYSCON_TRACE_REPLY, first);
    }

    if(rx_count < 4)
        goto cleanup;
    rlen = rx[1];
    if(rlen < 3 || rlen >= 16 || rx_count < rlen + 1)
        goto cleanup;
    sum = 0;
    for(i = 0; i < rlen; i++)
        sum += rx[i];
    if(rx[rlen] != (uint8_t)(~sum & 0xFF))
        goto cleanup;

    /* Publish before classifying the result code below: the status byte
     * describes the machine, not the command, so a refusal still carries it. */
    sc_reply_len = rlen + 1;
    for(i = 0; i < sc_reply_len; i++)
        sc_reply[i] = rx[i];
    sc_reply_seq++;

    /* 0x80/0x81 request Sony's special command-only second phase, not a replay
     * of the original parameterized command.  Return an internal sentinel only
     * after the response length and checksum have both been validated. */
    if(rx[2] == 0x82)
        result = 0;
    else if(tx[0] >= 0x20 && (rx[2] == 0x80 || rx[2] == 0x81))
        result = SC_RESULT_RETRY;
    else if(tx[0] < 0x20 && !(rx[2] & 0x80))
        result = 0; /* data response rather than a high-bit status code */

cleanup:
    SC(SC_CR1) = 4;
    GPIO_CLEAR = GPIO_TX_PIN;
    sc_sync();
    return result;
}

/* Send one command packet.
 *
 * Sequence taken instruction-for-instruction from Sony's transmit path at
 * syscon.prx text+0x22f0.  The previous implementation here got four things
 * wrong and therefore never sent a valid packet at all:
 *
 *   - wrote (len+1) FIFO *words* instead of ceil((len+1)/2); a 4-byte packet
 *     went out as 8 bytes
 *   - wrote 1 to register 0x20; Sony writes 3, and does it BEFORE the data
 *   - wrote 4 to SSPCR1 to start; Sony writes 6 (4 leaves the SSP disabled)
 *   - omitted the GPIO gating entirely
 *
 * Packet layout: [cmd][len][params...][hash], where len counts cmd+len+params
 * and hash is ~sum of those bytes.  Bytes go out as big-endian 16-bit words, so
 * `31 03 00 CB` becomes the words 0x3103, 0x00CB.
 */
int psp_syscon_cmd(uint8_t cmd, const uint8_t *params, int nparams) {
    uint8_t buf[16];
    uint8_t retry[4];
    int i, len, total, result;
    unsigned int sum;

    if(!inited)
        psp_syscon_init();
    if(nparams < 0 || nparams > 12 || (nparams && !params))
        return -1;

    for(i = 0; i < 16; i++)
        buf[i] = 0xFF; /* tail padding for the final half-word */

    len = 2 + nparams;
    buf[0] = cmd;
    buf[1] = (uint8_t)len;
    for(i = 0; i < nparams; i++)
        buf[2 + i] = params[i];

    sum = 0;
    for(i = 0; i < len; i++)
        sum += buf[i];
    buf[len] = (uint8_t)(~sum & 0xFF);

    total = len + 1;            /* bytes on the wire, including the hash */

    result = sc_exchange(buf, total,
                         cmd >= 0x20 ? SC_SETTLE_COUNT_TICKS : 0);

    /* Retail preserves the original packet after a checksum-valid 0x80/0x81
     * response, waits, then sends only [cmd, 0x02, ~(cmd+2)].  For watchdog
     * command 0x31 the two FIFO words are 0x3102 and 0xCCFF.  Never repeat the
     * original parameterized packet: it may already have taken effect. */
    retry[0] = cmd;
    retry[1] = 2;
    retry[2] = (uint8_t)(~((unsigned int)cmd + 2u) & 0xFF);
    retry[3] = 0xFF;
    for(i = 0; result == SC_RESULT_RETRY && i < SC_RETRY_LIMIT; i++) {
        uint32_t wire = ((uint32_t)retry[0] << 24) |
                        ((uint32_t)retry[1] << 16) |
                        ((uint32_t)retry[2] << 8) | retry[3];
        psp_syscon_trace(PSP_SYSCON_TRACE_RETRY, wire);
        result = sc_exchange(retry, 3, SC_RETRY_COUNT_TICKS);
    }
    if(result == SC_RESULT_RETRY)
        result = SC_ERR_STATUS;
    psp_syscon_trace(PSP_SYSCON_TRACE_RESULT, (uint32_t)result);
    return result;
}

void psp_syscon_backlight(bool on) {
    /* LED-control command (0x47); the display/backlight LED bit is 0x10.  A set
     * bit turns that rail on.  See PROVENANCE — the backlight bit is inferred. */
    uint8_t param = on ? 0x10 : 0x00;
    psp_syscon_cmd(0x47, &param, 1);
}

/* Keep-alive commands.
 *
 * Syscon holds the power rail and cuts it if the main CPU goes quiet; on
 * hardware the console powers off 10-15 s after the firmware stops running,
 * even with the CPU healthy and merely spinning.  Something has to keep talking
 * to it.  Which command resets that timeout is not known, so this sends every
 * candidate that is (a) parameterless, so it cannot have an argument-dependent
 * side effect, and (b) attested in BOTH Sony's bare-metal IPL stage 2 and the
 * sceSYSCON_Driver of 6.61 -- i.e. routine traffic on this hardware.
 *
 * Recovered by tracing the packet builder (command byte at struct+12, length at
 * +13; length 2 means no parameters):
 *
 *     0x11  len 2   IPL + driver
 *     0x61  len 2   IPL + driver
 *
 * 0x07 (IPL only) and 0x60 / 0x6d (driver only) are the next candidates if
 * neither of these is the one.  Narrow the list once something works. */
#ifndef PSP_SYSCON_KEEPALIVE_CMDS
#define PSP_SYSCON_KEEPALIVE_CMDS { 0x11, 0x61 }
#endif

void psp_syscon_keepalive(void) {
    static const uint8_t cmds[] = PSP_SYSCON_KEEPALIVE_CMDS;

    if(!inited)
        psp_syscon_init();
    for(unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        (void)psp_syscon_cmd(cmds[i], 0, 0);
}

/* Tachyon watchdog control (see syscon.h).  Parameter 0x00 = disabled; any
 * 1..0x7F timeout would be sent as `value | 0x80`. */
#define SYSCON_CMD_CTRL_TACHYON_WDT 0x31

int psp_syscon_wdt_disable(void) {
    static const uint8_t off = 0x00;

    if(!inited)
        psp_syscon_init();
    return psp_syscon_cmd(SYSCON_CMD_CTRL_TACHYON_WDT, &off, 1);
}

/* ===== Status byte, power switch and power off ===========================*/

#define SYSCON_CMD_NOP                 0x00 /* sceSysconNop, no parameters   */
#define SYSCON_CMD_GET_BARYON_VERSION  0x01
#define SYSCON_CMD_POWER_STANDBY       0x35

/* The 16-bit parameter newer baryons expect with 0x35.  power_04g.prx
 * initialises the field it passes to 8 (text+0x11f0), so 8 is retail's default;
 * its meaning is not RE'd. */
#define PSP_SYSCON_STANDBY_PARAM       0x0008

bool psp_syscon_status(uint8_t *status) {
    if(sc_reply_seq == 0)
        return false;
    if(status)
        *status = sc_reply[0];
    return true;
}

int psp_syscon_poll_status(uint8_t *status) {
    uint32_t seq = sc_reply_seq;
    int result;

    if(!inited)
        psp_syscon_init();

    /* Any reply that passed the checksum carries a usable status byte, so
     * success is "the sequence moved", not "result == 0". */
    result = psp_syscon_cmd(SYSCON_CMD_NOP, 0, 0);
    if(sc_reply_seq == seq)
        return result ? result : -1;
    if(status)
        *status = sc_reply[0];
    return 0;
}

int psp_syscon_baryon_version(uint32_t *version) {
    uint32_t seq = sc_reply_seq;
    uint32_t value = 0;
    int i, payload;

    if(!version)
        return -1;
    if(!inited)
        psp_syscon_init();

    if(psp_syscon_cmd(SYSCON_CMD_GET_BARYON_VERSION, 0, 0) != 0)
        return -1;
    if(sc_reply_seq == seq)
        return -1;

    /* [status][len][code][payload...][hash], len counting all but the hash, so
     * the payload is bytes 3..len-1, little-endian (syscon.prx text+0x1c5c). */
    payload = sc_reply_len - 1 - 3;
    if(payload < 1 || payload > 4)
        return -1;
    for(i = 0; i < payload; i++)
        value |= (uint32_t)sc_reply[3 + i] << (8 * i);

    *version = value;
    return 0;
}

int psp_syscon_power_standby(void) {
    uint32_t version = 0;
    uint8_t  params[2];

    if(!inited)
        psp_syscon_init();

    /* Guessing the packet form is worse than not sending one. */
    if(psp_syscon_baryon_version(&version) != 0)
        return -1;

    if((((version >> 16) & 0xF0u) < 0x30u))
        return psp_syscon_cmd(SYSCON_CMD_POWER_STANDBY, 0, 0);

    params[0] = (uint8_t)(PSP_SYSCON_STANDBY_PARAM & 0xFFu);
    params[1] = (uint8_t)((PSP_SYSCON_STANDBY_PARAM >> 8) & 0xFFu);
    return psp_syscon_cmd(SYSCON_CMD_POWER_STANDBY, params, 2);
}
