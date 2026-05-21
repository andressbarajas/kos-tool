/* client/wii/net/kosload.h */
#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <stdbool.h>
#include <kosload/protocol.h>

#define KOSLOAD_PMCR 1

#define BBA_BG_COLOR   0x00100530
#define ENC_BG_COLOR   0x00100530
#define W5500_BG_COLOR 0x00100530
#define ERROR_BG_COLOR 0x00400000
#define STR_COLOR      0x00FFFFFF

#define WII_TBR_FREQUENCY 60750000u
#define PERFCOUNTER_SCALE WII_TBR_FREQUENCY

#define BBA_MODEL ADAPTER_WII_LAN
#define ENC_MODEL ADAPTER_WII_LAN
#define W5500_MODEL ADAPTER_WII_LAN

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
extern void update_lease_time_display(unsigned int new_time);
extern void uint_to_string_dec(unsigned int foo, char *bar);
extern void dhcp_poll(void);

#endif /* __KOSLOAD_H__ */
