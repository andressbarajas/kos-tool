/* client/include/kosload/screensaver.h */
/*
 * Platform-independent screensaver for kosload.
 *
 * Bouncing white box on a black screen, activates after 30 seconds
 * of inactivity. Also wakes on controller/keyboard input.
 *
 * Uses target_ops_t for all hardware access (timer, video, input).
 */

#ifndef KOSLOAD_SCREENSAVER_H
#define KOSLOAD_SCREENSAVER_H

#include <stdbool.h>

/* Initialize the screensaver and start the idle timer.
 * restore_cb is called when the screensaver deactivates to restore
 * the normal screen (transport-specific).
 * bg_color is the background color to use (platform-specific format). */
void screensaver_init(void (*restore_cb)(void), unsigned int bg_color);

/* Call from idle loops. Checks the timer and activates the screensaver
 * after 30 seconds, then animates the bouncing box each call.
 * Also polls controller/keyboard input to auto-wake. */
void screensaver_poll(void);

/* Wake from screensaver. If active, calls the restore callback and
 * resets the timer. Returns true if the screensaver was active. */
bool screensaver_wake(void);

/* Returns true if the screensaver is currently active. */
bool screensaver_is_active(void);

/* Reset the idle timer without restoring the screen.
 * Call after processing a command to restart the 30-second countdown. */
void screensaver_reset(void);

#endif /* KOSLOAD_SCREENSAVER_H */
