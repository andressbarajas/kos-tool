/* client/playstation2/power.c — front power button polling and power off.
 *
 * The Mechacon owns the button: it debounces it, times the hold, then
 * latches one request on CDVD ISTAT bit 2 and waits for Mechacon S command
 * 0x0F.  This file answers that; iop/smap_irx.c is the IOP half.
 *
 * There is deliberately no hold timer here: no register reports the
 * button's position, so there is nothing to sample and the Mechacon has
 * already done that work.  Arming therefore watches for the absence of the
 * event instead of for a released button — if ISTAT bit 2 were the wrong
 * bit and read as permanently set, the console would simply never power
 * off.
 *
 * Polled only from the network transport's idle path, and only once the
 * link has gone quiet, so a press cannot interrupt a transfer or a
 * firmware update.
 */

#include <kosload/display.h>
#include <kosload/target.h>

#include "iop_smap.h"
#include "net/kosload.h"
#include "power.h"

/* Poll interval, and how long the link must have been quiet first.  The
 * host paces PS2 uploads in the low milliseconds, so a transfer in progress
 * never produces a gap this long; an idle loader produces nothing else. */
#ifndef PS2_POWER_POLL_MS
#define PS2_POWER_POLL_MS   100
#endif
#ifndef PS2_POWER_QUIET_MS
#define PS2_POWER_QUIET_MS  400
#endif

/* Consecutive request-free polls required before a request is acted on. */
#ifndef PS2_POWER_ARM_SAMPLES
#define PS2_POWER_ARM_SAMPLES 4
#endif

/* How long to let the Mechacon drop the rail before deciding it declined. */
#define PS2_POWER_OFF_GRACE_MS 2000

/* Row for messages this file owns.  Kept off the status row, which the
 * main loop repaints every iteration. */
#define PS2_POWER_NOTICE_Y  222

static uint64_t last_poll;
static uint64_t last_activity;
static bool     timers_started;
static bool     armed;         /* seen ARM_SAMPLES quiet polls */
static unsigned quiet_polls;

/* Divide the 32-bit rate first so this needs no 64-bit divide. */
static uint64_t ps2_power_ms_to_ticks(const target_ops_t *t, uint32_t ms) {
    return (uint64_t)((t->ticks_per_second / 1000u) * ms);
}

void ps2_power_note_activity(void) {
    const target_ops_t *t = target_get_ops();

    last_activity = t->get_ticks();
    timers_started = true;
}

void ps2_power_off(void) {
    const target_ops_t *t = target_get_ops();
    uint32_t            status = 0;
    uint64_t            start;
    int                 rc;

    /* Drawn first so the press is acknowledged even if 0x0F is refused. */
    disp_status("powering off...");

    rc = ps2_smap_power_off(&status);
    if(rc == 0) {
        /* Spin rather than return, so the loader does not answer host
         * traffic with the rail collapsing underneath it. */
        start = t->get_ticks();
        while((t->get_ticks() - start) < ps2_power_ms_to_ticks(t, PS2_POWER_OFF_GRACE_MS))
            ;
    }

    /* Declined.  Nothing else on this console can cut the rail, so report
     * it rather than sitting silent. */
    clear_lines(PS2_POWER_NOTICE_Y, 24, global_bg_color);
    t->draw_string(NETWORK_DISPLAY_X, PS2_POWER_NOTICE_Y,
                   "power off refused by mechacon", 0xffff);
}

void ps2_power_poll(void) {
    const target_ops_t *t = target_get_ops();
    uint64_t            now = t->get_ticks();
    uint32_t            requested = 0;

    if(!timers_started) {
        last_poll = now;
        last_activity = now;
        timers_started = true;
        return;
    }

    if((now - last_poll) < ps2_power_ms_to_ticks(t, PS2_POWER_POLL_MS))
        return;
    if((now - last_activity) < ps2_power_ms_to_ticks(t, PS2_POWER_QUIET_MS))
        return;
    last_poll = now;

    /* No usable answer — say nothing rather than guess at the button. */
    if(ps2_smap_power_poll(&requested, 0) != 0)
        return;

    if(!requested) {
        if(quiet_polls < PS2_POWER_ARM_SAMPLES) {
            quiet_polls++;
            if(quiet_polls >= PS2_POWER_ARM_SAMPLES)
                armed = true;
        }
        return;
    }

    /* A request before the bit has been seen quiet: either latched before
     * the loader started, or not the bit we think it is.  Drop it. */
    if(!armed) {
        quiet_polls = 0;
        return;
    }

    ps2_power_off();

    /* Declined.  Re-arm from scratch rather than leave a hair trigger. */
    armed = false;
    quiet_polls = 0;
}
