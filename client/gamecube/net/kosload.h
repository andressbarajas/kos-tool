/* client/gamecube/net/kosload.h */
/*
 * GameCube display/timing constants for the network stack.
 * Shared state and display helpers live in <kosload/display.h>.
 */

#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <kosload/display.h>
#include <kosload/protocol.h>

/* Colors in 0x00RRGGBB format (converted to YUV by gc_color_to_yuy2) */
#define BBA_BG_COLOR   0x00200040 /* GameCube purple (dark) */
#define ENC_BG_COLOR   0x00003020 /* Dark teal (ENC28J60) */
#define W5500_BG_COLOR 0x00002030 /* Dark teal-blue (W5500) */
#define STR_COLOR      0x00FFFFFF /* White */

/* GC Gekko TBR frequency for timing calculations.
 * TBR runs at bus_clock/4 = 162MHz/4 = 40.5MHz. */
#define PERFCOUNTER_SCALE GC_TBR_FREQUENCY

#define W5500_MODEL ADAPTER_GC_W5500

#endif /* __KOSLOAD_H__ */
