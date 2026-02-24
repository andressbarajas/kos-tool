/* client/gamecube/si.c */
/*
 * Minimal GameCube Serial Interface (SI) driver for controller polling.
 *
 * The SI bus connects to the 4 controller ports. This implementation
 * only supports polling standard controllers for button state — enough
 * to detect input for screensaver wake.
 *
 * Reference: YAGCD (Yet Another GameCube Documentation)
 */

#include <stdint.h>

/* SI register base */
#define SI_BASE  0xCC006400

/* Per-channel registers (3 regs * 4 bytes = 0x0C apart) */
#define SI_CH_OUTBUF(ch)  (*(volatile uint32_t *)(SI_BASE + (ch) * 0x0C + 0x00))
#define SI_CH_INBUFH(ch)  (*(volatile uint32_t *)(SI_BASE + (ch) * 0x0C + 0x04))
#define SI_CH_INBUFL(ch)  (*(volatile uint32_t *)(SI_BASE + (ch) * 0x0C + 0x08))

/* Global SI registers */
#define SI_POLL    (*(volatile uint32_t *)(SI_BASE + 0x30))
#define SI_COMCSR  (*(volatile uint32_t *)(SI_BASE + 0x34))
#define SI_STATUS  (*(volatile uint32_t *)(SI_BASE + 0x38))

/* SI_COMCSR bits */
#define SI_COMCSR_TCINT      (1 << 29)  /* Transfer complete interrupt status */
#define SI_COMCSR_TSTART     (1 << 0)   /* Transfer start */

int si_poll_controller(int channel)
{
    /* Write poll command (big-endian byte order in OUTBUF):
     * Byte 0: 0x40 = controller poll command
     * Byte 1: 0x03 = mode 3 (buttons + analog)
     * Byte 2: 0x00 = rumble off */
    SI_CH_OUTBUF(channel) = 0x40030000;

    /* Clear transfer complete, set channel, output 3 bytes, input 8 bytes, start */
    SI_COMCSR = SI_COMCSR_TCINT |           /* Clear TC interrupt */
                ((uint32_t)channel << 1) |  /* Channel select */
                (2 << 16) |                 /* Output length - 1 = 2 (3 bytes) */
                (7 << 20) |                 /* Input length - 1 = 7 (8 bytes) */
                SI_COMCSR_TSTART;           /* Start transfer */

    /* Wait for transfer complete */
    while (!(SI_COMCSR & SI_COMCSR_TCINT))
        ;

    /* Check for communication error (no controller connected).
     * Each channel has 8 status bits; NOREP is bit 3 of the channel byte.
     * Channel byte positions: ch0=bits 31-24, ch1=23-16, ch2=15-8, ch3=7-0. */
    uint32_t status = SI_STATUS;
    uint32_t ch_err_mask = (uint32_t)0xF8 << (24 - channel * 8);  /* error bits for this channel */
    if (status & ch_err_mask)
        return 0;

    /* Read response: high word contains button data.
     * Byte 0: [ErrStat|ErrLatch|Origin|Start|Y|X|B|A]
     * Byte 1: [Always1|L|R|Z|DUp|DDown|DRight|DLeft]
     * Mask out non-button bits (15-13 = error/origin, 7 = always set). */
    uint32_t in_h = SI_CH_INBUFH(channel);
    uint16_t buttons = (uint16_t)(in_h >> 16) & 0x1F7F;

    return (buttons != 0) ? 1 : 0;
}
