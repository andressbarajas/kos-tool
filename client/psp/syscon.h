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

/* ===== Status byte, power switch and power off =============================
 *
 * Syscon reports the switches and supply flags in byte 0 of EVERY reply, which
 * sc_exchange() already receives and checksums -- so reading the power switch
 * costs no more than sending any command.
 *
 * Bit 4, ACTIVE HIGH; measured on a PSP-1000 as 0x08 idle, 0x18 held.  RE'd
 * from 6.61 syscon.prx: text+0x2434 returns the reply's first byte, the ISR
 * dispatches bit 4 uninverted to callback slot K=92, and power_04g.prx's
 * handler there (text+0x2ee0) sets scePower 0x80000000 = POWERSWITCH.  Slot
 * K = 80 + 12*idx: the ISR loads the handler from K+12, and dropping that +12
 * shifts every bit by one slot. */
#define PSP_SYSCON_STATUS_POWER_SWITCH 0x10

static inline bool psp_syscon_power_switch_held(uint8_t status) {
    return (status & PSP_SYSCON_STATUS_POWER_SWITCH) != 0;
}

/* Status byte from the most recent checksum-valid reply.  False if no reply has
 * ever validated, in which case *status is untouched. */
bool psp_syscon_status(uint8_t *status);

/* Fetch a fresh status byte with the syscon NOP (command 0x00, no parameters --
 * what sceSysconNop at syscon.prx text+0x2d88 builds).  Below 0x20, so it skips
 * the settle delay and is cheap to poll.  Returns 0 and stores the status, else
 * the psp_syscon_cmd() result or -1. */
int psp_syscon_poll_status(uint8_t *status);

/* Read the baryon (syscon MCU) version, command 0x01.  The reply payload is
 * little-endian, so the generation nibble retail tests lands in bits 20..23. */
int psp_syscon_baryon_version(uint32_t *version);

/* Power the console off: syscon command 0x35.  sceSysconPowerStandby (syscon.prx
 * text+0x2b40) picks the packet form from the baryon version -- `35 02 C8` with
 * no parameters below 0x30 (PSP-1000/2000), `35 04 lo hi <hash>` at or above it
 * (PSP-3000; see PSP_SYSCON_STANDBY_PARAM in syscon.c).  Both confirmed on
 * hardware.
 *
 * Not the 0x32 reset that never worked: that one is gated on a physical input
 * and answers 0x83 when the gate is closed.  Non-zero means the packet was
 * refused, so the caller needs a fallback; on success it should spin, as
 * retail's call site does. */
int psp_syscon_power_standby(void);

/* Send a harmless command purely to prove the CPU is still alive.
 *
 * Syscon holds the power rail and cuts it if the main CPU goes quiet -- on
 * hardware the console powers off 10-15 s after the firmware stops running,
 * even if the CPU is otherwise healthy and simply spinning.  A bare-metal
 * loader therefore has to keep talking to it.  Rate-limit calls; once every
 * couple of seconds is far more often than the timeout requires. */
void psp_syscon_keepalive(void);

#endif /* KOSLOAD_PSP_SYSCON_H */
