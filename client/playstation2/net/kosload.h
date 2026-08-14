/* client/playstation2/net/kosload.h */
/*
 * PlayStation 2 display/timing constants for the network stack.
 * Shared state and display helpers live in <kosload/display.h>.
 */
#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <kosload/display.h>

/* Colors in 0x00RRGGBB format */
#define BBA_BG_COLOR   0x001C81B3 /* PlayStation 2 blue */
#define STR_COLOR      0x00FFFFFF /* White */

/* SMAP uses same BG as BBA on PS2 */
#define SMAP_BG_COLOR  BBA_BG_COLOR

/* Shared network UI coordinates, tuned for PS2's visible NTSC area. */
#define NETWORK_DISPLAY_X        48
#define NETWORK_DHCP_ATTEMPTS_Y  370
#define NETWORK_DHCP_LEASE_Y     394

#define PERFCOUNTER_SCALE 294912000

#endif
