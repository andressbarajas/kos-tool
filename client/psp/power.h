/* client/psp/power.h — power-switch handling for the PSP loader.
 *
 * Gives the power switch its normal meaning back: holding it powers the console
 * off, instead of waiting on syscon's ten-second kill.  How long is
 * PSP_POWER_HOLD_MS in power.c, one second by default.  Loader-only -- the
 * stage-1 stub links syscon.c but not this file.
 */

#ifndef KOSLOAD_PSP_POWER_H
#define KOSLOAD_PSP_POWER_H

/* Rate-limited power-switch poll, cheap to call from a spin loop.  Does not
 * return once the switch has been held long enough. */
void psp_power_poll(void);

/* Shut the console down now.  Does not return. */
void psp_power_off(void);

#endif /* KOSLOAD_PSP_POWER_H */
