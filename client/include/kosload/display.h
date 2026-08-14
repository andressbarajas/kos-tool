/* client/include/kosload/display.h
 *
 * Loader status display: shared state, the on-screen status helpers, and the
 * video primitives every console port must supply.
 *
 * Consolidated from the per-console client/<console>/net/kosload.h files,
 * which each restated this same block.  Those headers now carry only genuine
 * per-console values (background colours, counter frequencies, UI coordinates)
 * and include this one.
 *
 * Ownership is split:
 *   - The state, the disp_ helpers and dhcp_poll are DEFINED in shared code
 *     (client/common/network/entry.c), so the consoles were previously
 *     declaring symbols they do not own.
 *   - uint_to_string/clear_lines/setup_video are the console-side contract,
 *     DEFINED per port in client/<console>/video.c.
 */
#ifndef KOSLOAD_DISPLAY_H
#define KOSLOAD_DISPLAY_H

#include <stdbool.h>

/* ===== Console-provided video primitives (client/<console>/video.c) ===== */

/* Hex-formats value into buf; buf must hold 8 digits plus a NUL. */
void uint_to_string(unsigned int value, unsigned char *buf);

/* Fills height scanlines starting at row y with color. */
void clear_lines(int y, int height, unsigned int color);

/* Brings up the framebuffer in mode and paints it color. */
void setup_video(unsigned int mode, unsigned int color);

/* ===== Shared loader state (client/common/network/entry.c) ===== */

extern volatile bool booted;
extern volatile bool running;
extern volatile bool receiving;
extern unsigned int global_bg_color;
extern volatile unsigned int installed_adapter;

/* ===== Network status display (client/common/network/entry.c) ===== */

void disp_info(void);
void disp_status(const char *status);
void disp_dhcp_attempts_count(void);
void disp_dhcp_next_attempt(unsigned int time_left);
void update_lease_time_display(unsigned int new_time);
void uint_to_string_dec(unsigned int value, char *buf);
void dhcp_poll(void);

#endif /* KOSLOAD_DISPLAY_H */
