/* client/gamecube/net/dcload.h */
/*
 * Compatibility header for ported dcload-ip network stack files.
 * GameCube version: different constants and CPU frequency.
 */

#ifndef __DCLOAD_H__
#define __DCLOAD_H__

#include <stdbool.h>

/* Performance counter configuration (maps to TBR on GC, used by adapter timeout loops) */
#define DCLOAD_PMCR 1

/* Colors in 0x00RRGGBB format (converted to YUV by gc_color_to_yuy2) */
#define BBA_BG_COLOR   0x00200040   /* GameCube purple (dark) */
#define ENC_BG_COLOR   0x00003020   /* Dark teal (ENC28J60) */
#define W5500_BG_COLOR 0x00002030   /* Dark teal-blue (W5500) */
#define ERROR_BG_COLOR 0x00400000   /* Dark red */
#define STR_COLOR      0x00FFFFFF   /* White */

/* GC Gekko TBR frequency for timing calculations.
 * TBR runs at bus_clock/4 = 162MHz/4 = 40.5MHz. */
#define PERFCOUNTER_SCALE GC_TBR_FREQUENCY

/* Adapter model IDs */
#define BBA_MODEL   0x0600
#define ENC_MODEL   0x0601
#define W5500_MODEL 0x0700

/* Global state (defined in entry.c) */
extern volatile bool booted;
extern volatile bool running;
extern volatile bool receiving;
extern unsigned int global_bg_color;
extern volatile unsigned int installed_adapter;

/* Functions provided by video.c */
extern const char *exception_code_to_string(unsigned int expevt);
extern void uint_to_string(unsigned int foo, unsigned char *bar);
extern void setup_video(unsigned int mode, unsigned int color);

/* Functions provided by entry.c (display) */
extern void disp_info(void);
extern void disp_status(const char *status);
extern void disp_dhcp_attempts_count(void);
extern void disp_dhcp_next_attempt(unsigned int);
extern void uint_to_string_dec(unsigned int foo, char *bar);
extern void dhcp_poll(void);

#endif /* __DCLOAD_H__ */
