/* client/psp/syscon.h — PSP syscon (system controller) serial interface.
 *
 * Bare-metal driver for the syscon SSP at 0xBE580000 — the ARM PrimeCell
 * synchronous-serial port that talks to the SC microcontroller.  Used here to
 * turn the LCD backlight on for a cold-boot visible panel, with no firmware
 * syscall.
 */

#ifndef KOSLOAD_PSP_SYSCON_H
#define KOSLOAD_PSP_SYSCON_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up the SSP (clock gate + controller init). */
void psp_syscon_init(void);

/* Send one syscon command packet [cmd, len, params..., hash].  The synchronous
 * path is the same one used by Sony's IPL: it polls GPIO4 for completion, then
 * consumes and validates the response packet.  Status.IE may be clear, but the
 * caller must own the SSP/GPIO handshake (the firmware's syscon ISR must not be
 * allowed to race this transaction).
 *
 * Returns 0 for an accepted response, -1 for invalid arguments, 1 if a stale
 * RX FIFO could not be drained, 2 if GPIO4 never reported completion, or 3 for
 * a controller/response/checksum error. */
int psp_syscon_cmd(uint8_t cmd, const uint8_t *params, int nparams);

/* Optional diagnostic hook.  syscon.c supplies a weak no-op implementation;
 * the stage-1 on-screen diagnostic overrides it to render each checkpoint.
 * Values are raw register contents unless the event says otherwise. */
enum psp_syscon_trace_event {
    PSP_SYSCON_TRACE_CLK_SELECT = 1,
    PSP_SYSCON_TRACE_CLK_ENABLE,
    PSP_SYSCON_TRACE_IO_ENABLE,
    PSP_SYSCON_TRACE_CR0,
    PSP_SYSCON_TRACE_CR1,
    PSP_SYSCON_TRACE_SR_INITIAL,
    PSP_SYSCON_TRACE_DRAIN,
    PSP_SYSCON_TRACE_TX,
    PSP_SYSCON_TRACE_GPIO4,
    PSP_SYSCON_TRACE_RX,
    PSP_SYSCON_TRACE_REPLY,
    PSP_SYSCON_TRACE_RESULT,
    PSP_SYSCON_TRACE_RETRY
};

void psp_syscon_trace(unsigned int event, uint32_t value);

/* Turn the LCD backlight on/off (syscon LED-control command). */
void psp_syscon_backlight(bool on);

/* Disable the Tachyon watchdog timer.
 *
 * Syscon runs a watchdog that the firmware enables and resets from a timer.  A
 * bare-metal loader stops the firmware, so nothing resets it, and syscon cuts
 * power 10-15 s later -- which looks exactly like a crash but is the console
 * deliberately killing a system it believes has hung.
 *
 * RE'd from sceSysconCtrlTachyonWDT (6.61 syscon.prx text+0x2a40): command 0x31
 * with a single parameter byte of `wdt | 0x80`, except that a wdt of 0 sends a
 * bare 0x00 -- clearing the enable bit rather than setting a timeout.  So one
 * command retires the watchdog for good; no periodic servicing needed. */
int psp_syscon_wdt_disable(void);

/* Send a harmless command purely to prove the CPU is still alive.
 *
 * Syscon holds the power rail and cuts it if the main CPU goes quiet -- on
 * hardware the console powers off 10-15 s after the firmware stops running,
 * even if the CPU is otherwise healthy and simply spinning.  A bare-metal
 * loader therefore has to keep talking to it.  Rate-limit calls; once every
 * couple of seconds is far more often than the timeout requires. */
void psp_syscon_keepalive(void);

#endif /* KOSLOAD_PSP_SYSCON_H */
