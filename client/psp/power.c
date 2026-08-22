/* client/psp/power.c — power-switch polling and bare-metal power off.
 *
 * The power switch never reaches the Allegrex; syscon reads it and tells the
 * firmware, so with the firmware stopped nothing answers it and the console
 * only goes down via syscon's ten-second hardware kill.  syscon does report the
 * switch in byte 0 of every reply, though, so the loader can have it back for
 * one NOP command per poll.  See syscon.h for the bit and the packet.
 *
 * Polled only from the transport's idle wait: letting the switch interrupt a
 * transfer or a firmware update would be worse than making you wait for it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "power.h"
#include "syscon.h"
#include "video.h"

/* COP0 Count rate, matching PSP_TICKS_PER_SEC in target.c (measured; see the
 * note there).  Coarse intervals only, so drift does not matter. */
#define PSP_COUNT_HZ            333000000

/* How long the switch must be held before the console goes down, and how often
 * it is sampled.  Override the hold with -DPSP_POWER_HOLD_MS=<ms>; a deliberate
 * hold rather than a flick keeps a brushed switch from ending a session.
 *
 * The hold is timed from the first held sample, not counted in samples, so poll
 * jitter cannot stretch or shrink it.  Both bounds come from the 32-bit COP0
 * Count, which wraps about every 12.9 s at this clock -- an unsigned delta
 * cannot measure past that. */
#ifndef PSP_POWER_HOLD_MS
#define PSP_POWER_HOLD_MS       1000
#endif
#define PSP_POWER_POLL_MS       50

#define POWER_POLL_TICKS        ((PSP_COUNT_HZ / 1000) * PSP_POWER_POLL_MS)
#define POWER_HOLD_TICKS        ((PSP_COUNT_HZ / 1000) * PSP_POWER_HOLD_MS)

_Static_assert(PSP_POWER_HOLD_MS <= 12000,
               "PSP_POWER_HOLD_MS exceeds the COP0 Count wrap");

/* How long to let syscon act on the standby command before falling back. */
#define POWER_STANDBY_GRACE_TICKS (2 * PSP_COUNT_HZ)

static uint32_t last_poll;
static bool     poll_started;
static bool     armed;          /* switch seen released at least once */
static bool     holding;        /* switch held since held_since */
static uint32_t held_since;

static inline uint32_t power_count(void) {
    uint32_t value;
    __asm__ volatile("mfc0 %0, $9" : "=r"(value));
    return value;
}

void psp_power_off(void) {
    uint32_t start;

    /* The transport's "idle..." line; drawn first so the switch is acknowledged
     * even if the shutdown below has to fall back. */
    psp_video_draw_string(30, 78, "powering off...", 0xffff);

    if(psp_syscon_power_standby() == 0) {
        /* Retail sends the command and spins until the rail drops.  Bound it so
         * a standby that does nothing still reaches the fallback. */
        start = power_count();
        while((uint32_t)(power_count() - start) < POWER_STANDBY_GRACE_TICKS)
            ;
    }

    /* Standby refused or ineffective.  Re-arm the Tachyon watchdog stub.S
     * retired at boot (`timeout | 0x80`) and stop talking to syscon: that is the
     * documented 10-15 s cut, so the console still ends up off. */
    {
        static const uint8_t wdt_shortest = 0x81;
        (void)psp_syscon_cmd(0x31, &wdt_shortest, 1);
    }

    for(;;)
        __asm__ volatile("");
}

void psp_power_poll(void) {
    uint32_t now = power_count();
    uint8_t  status;

    if(poll_started && (uint32_t)(now - last_poll) < POWER_POLL_TICKS)
        return;
    last_poll = now;
    poll_started = true;

    /* No valid reply — say nothing rather than guess a switch state. */
    if(psp_syscon_poll_status(&status) != 0)
        return;

    if(!psp_syscon_power_switch_held(status)) {
        /* Arm only after seeing the switch released, so one still held from
         * launching the loader cannot shut the console straight back down. */
        armed = true;
        holding = false;
        return;
    }

    if(!armed)
        return;

    if(!holding) {
        holding = true;
        held_since = now;
        return;
    }

    if((uint32_t)(now - held_since) < POWER_HOLD_TICKS)
        return;

    psp_power_off();
}
