/* client/playstation2/net/kosload.h */
#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <stdbool.h>

#define KOSLOAD_PMCR 1

/* Colors in 0x00RRGGBB format */
#define BBA_BG_COLOR   0x001C81B3   /* PlayStation 2 blue */
#define STR_COLOR      0x00FFFFFF   /* White */

/* SMAP uses same BG as BBA on PS2 */
#define SMAP_BG_COLOR  BBA_BG_COLOR

/* Shared network UI coordinates, tuned for PS2's visible NTSC area. */
#define NETWORK_DISPLAY_X        48
#define NETWORK_DHCP_ATTEMPTS_Y  370
#define NETWORK_DHCP_LEASE_Y     394

#define PERFCOUNTER_SCALE 294912000

/* Global state (defined in entry.c) */
extern volatile bool booted;
extern volatile bool running;
extern volatile bool receiving;
extern unsigned int global_bg_color;
extern volatile unsigned int installed_adapter;

extern void uint_to_string(unsigned int foo, unsigned char *bar);
extern void setup_video(unsigned int mode, unsigned int color);

extern void disp_info(void);
extern void disp_status(const char *status);
extern void disp_dhcp_attempts_count(void);
extern void disp_dhcp_next_attempt(unsigned int);
extern void uint_to_string_dec(unsigned int foo, char *bar);
extern void dhcp_poll(void);

#endif
