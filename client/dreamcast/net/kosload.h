/* client/dreamcast/net/kosload.h */
/*
 * Dreamcast display/timing constants for the network stack.
 * Shared state and display helpers live in <kosload/display.h>.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/dcload.h
 */

#ifndef __KOSLOAD_H__
#define __KOSLOAD_H__

#include <kosload/display.h>
#include <kosload/protocol.h>

/* Performance counter used by the adapter timeout loops and cmd_pmcr. */
#define KOSLOAD_PMCR 1

/* Background colors (RGB565 format) — black like dcload-serial */
#define BBA_BG_COLOR   0x0010
#define LAN_BG_COLOR   0x0100
#define W5500_BG_COLOR 0x0210 /* Dark cyan-ish */
#define STR_COLOR      0xffff

/* SH4 CPU frequency (stock Dreamcast) */
#define SH4_FREQUENCY     (199496956)
#define PERFCOUNTER_SCALE SH4_FREQUENCY

#define W5500_MODEL ADAPTER_DC_W5500

/* Framebuffer color modes */
#define FB_RGB0555 0
#define FB_RGB565  1

/* Startup support */
extern void __call_builtin_sh_set_fpscr(unsigned int value);
extern void STARTUP_Init_Video(unsigned char fbuffer_color_mode);
extern void STARTUP_Set_Video(unsigned char fbuffer_color_mode);

#endif /* __KOSLOAD_H__ */
