/* client/dreamcast/net/dcload.h */
/*
 * Compatibility header for ported dcload-ip network stack files.
 * Provides the defines and declarations that dcload.h originally supplied.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/dcload.h
 */

#ifndef __DCLOAD_H__
#define __DCLOAD_H__

/* Performance counter configuration (used by adapter timeout loops and cmd_pmcr) */
#define DCLOAD_PMCR 1

/* Background colors (RGB565 format) — black like dcload-serial */
#define BBA_BG_COLOR   0x0010
#define LAN_BG_COLOR   0x0100
#define W5500_BG_COLOR 0x0210   /* Dark cyan-ish */
#define ERROR_BG_COLOR 0x2000
#define STR_COLOR      0xffff

/* SH4 CPU frequency (stock Dreamcast) */
#define SH4_FREQUENCY (199496956)
#define PERFCOUNTER_SCALE SH4_FREQUENCY

/* Adapter model IDs — must match legacy dcload-ip (octal values) */
#define LAN_MODEL   0300
#define BBA_MODEL   0400
#define W5500_MODEL 0500

/* Global state */
extern volatile unsigned char booted;
extern volatile unsigned char running;
extern volatile unsigned char receiving;
extern unsigned int global_bg_color;
extern volatile unsigned int installed_adapter;

/* Functions provided by video.c */
extern char *exception_code_to_string(unsigned int expevt);
extern void uint_to_string(unsigned int foo, unsigned char *bar);
extern void setup_video(unsigned int mode, unsigned int color);
extern void clear_lines(int y, int height, unsigned int color);

/* Functions provided by dcload main / display */
extern void disp_info(void);
extern void disp_status(const char *status);
extern void disp_dhcp_attempts_count(void);
extern void disp_dhcp_next_attempt(unsigned int);
extern void uint_to_string_dec(unsigned int foo, char *bar);
extern void dhcp_poll(void);

/* Framebuffer color modes */
#define FB_RGB0555 0
#define FB_RGB565  1

/* Startup support */
extern void __call_builtin_sh_set_fpscr(unsigned int value);
extern void STARTUP_Init_Video(unsigned char fbuffer_color_mode);
extern void STARTUP_Set_Video(unsigned char fbuffer_color_mode);

#endif /* __DCLOAD_H__ */
