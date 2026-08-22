/* client/playstation2/power.h — front power button for the PS2 loader.
 *
 * Holding the front button powers the console off.  The hold length is the
 * Mechacon's, not ours; see power.c.  Loader-only — the outer bootstrap
 * does not link this file. */

#ifndef KOSLOAD_PS2_POWER_H
#define KOSLOAD_PS2_POWER_H

/* Rate-limited poll; one IOP RPC at most every PS2_POWER_POLL_MS, and only
 * on a quiet link.  Does not return if the console powers off. */
void ps2_power_poll(void);

/* Tell the poll that packets are moving.  Called from the receive path. */
void ps2_power_note_activity(void);

/* Shut the console down now.  Returns only if the Mechacon declines. */
void ps2_power_off(void);

#endif /* KOSLOAD_PS2_POWER_H */
