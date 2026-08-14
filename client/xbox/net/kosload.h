/* client/xbox/net/kosload.h — Xbox display/timing constants.
 * Shared state and display helpers live in <kosload/display.h>. */
#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <kosload/display.h>

/* Colors in 0x00RRGGBB format. */
#define NVNET_BG_COLOR 0x00107C10 /* Xbox green */
#define STR_COLOR      0x00FFFFFF /* White */

/* Network UI coordinates, tuned for the 640x480 console. */
#define NETWORK_DISPLAY_X       30
#define NETWORK_DHCP_ATTEMPTS_Y 370
#define NETWORK_DHCP_LEASE_Y    394

#define PERFCOUNTER_SCALE 733333333

#endif /* __KOSLOAD_H__ */
