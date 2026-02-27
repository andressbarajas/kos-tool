/* client/gamecube/video.h */
/*
 * GameCube video output.
 *
 * Minimal VI (Video Interface) driver for displaying text on screen.
 * Uses NTSC 480i with a YUY2 external framebuffer (XFB).
 */

#ifndef KOSLOAD_GC_VIDEO_H
#define KOSLOAD_GC_VIDEO_H

#include <stdint.h>

/* VI register base */
#define VI_BASE             0xCC002000

/* XFB dimensions */
#define GC_SCREEN_WIDTH     640
#define GC_SCREEN_HEIGHT    480

/* Character cell size (12x24 font) */
#define GC_CHAR_WIDTH       12
#define GC_CHAR_HEIGHT      24

/* Initialize the VI for NTSC 480i output */
void gc_video_init(void);

/* Set the XFB address. Must be 32-byte aligned in MEM1. */
void gc_video_set_xfb(void *xfb);

/* Clear the framebuffer with a color (RGB565 format for interface compat) */
void gc_video_clear(uint32_t color);

/* Draw a character at pixel position (x, y) with the given color */
void gc_video_draw_char(int x, int y, char c, uint32_t color);

/* Draw a string at pixel position (x, y) with the given color */
void gc_video_draw_string(int x, int y, const char *str, uint32_t color);

/* Convert a 32-bit RGB color (0x00RRGGBB) to a YUY2 pixel pair (Y Cb Y Cr).
 * Returns a 32-bit value suitable for writing directly to the XFB. */
uint32_t gc_color_to_yuy2(uint32_t rgb);

/* Functions expected by crt0.S exception handler header */
void setup_video(uint32_t mode, uint32_t bg_color);
void clear_screen(uint32_t color);
void draw_string(int x, int y, const char *str, uint32_t color);
void uint_to_string(unsigned int val, unsigned char *buf);
const char *exception_code_to_string(uint32_t code);

#endif /* KOSLOAD_GC_VIDEO_H */
