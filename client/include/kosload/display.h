/* client/include/kosload/display.h
 *
 * Loader status display: shared state, the on-screen status helpers, and the
 * video contract every console port speaks.
 *
 * Consolidated from the per-console client/<console>/net/kosload.h files,
 * which each restated this same block.  Those headers now carry only genuine
 * per-console values (background colours, counter frequencies, UI coordinates)
 * and include this one.
 *
 * Everything declared here is DEFINED in shared code:
 *   - the flat video names in client/common/core/display.c, forwarding to the
 *     port's target_ops callbacks (see <kosload/target.h>);
 *   - the state, the disp_ helpers and dhcp_poll in client/common/network/entry.c.
 */
#ifndef KOSLOAD_DISPLAY_H
#define KOSLOAD_DISPLAY_H

#include <stdbool.h>

/* ===== Video contract (client/common/core/display.c) =====
 *
 * These are also the names the per-console entry header exports to guest
 * programs, so a port must keep them linked into every one of its binaries.
 */

/* Hex-formats value into buf as 8 digits; buf must hold 8 digits plus a NUL. */
void uint_to_string(unsigned int value, unsigned char *buf);

/* Fills height scanlines starting at row y with color. */
void clear_lines(int y, int height, unsigned int color);

/* Brings up the framebuffer in mode and paints it color. */
void setup_video(unsigned int mode, unsigned int color);

/* Paints the whole framebuffer color. */
void clear_screen(unsigned int color);

/* Draws str at pixel position (x, y) in color. */
void draw_string(int x, int y, const char *str, unsigned int color);

/* ===== Shared loader state (client/common/network/entry.c) ===== */

extern volatile bool booted;
extern volatile bool running;
extern volatile bool receiving;
extern unsigned int global_bg_color;
/* ADAPTER_* id of the live link */
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
