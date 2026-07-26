/* client/xbox/net/kosload.h — Xbox network display config + shared externs. */
#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <stdbool.h>

#define KOSLOAD_PMCR 1

/* Colors in 0x00RRGGBB format. */
#define NVNET_BG_COLOR 0x00107C10 /* Xbox green */
#define STR_COLOR      0x00FFFFFF /* White */

/* Network UI coordinates, tuned for the 640x480 console. */
#define NETWORK_DISPLAY_X       30
#define NETWORK_DHCP_ATTEMPTS_Y 370
#define NETWORK_DHCP_LEASE_Y    394

#define PERFCOUNTER_SCALE 733333333

/* Global state (defined in entry.c). */
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

#endif /* __KOSLOAD_H__ */
