/* client/playstation2/video.h */
/*
 * PlayStation 2 GS (Graphics Synthesizer) video output.
 *
 * Minimal GS driver for displaying text on screen using GIF DMA.
 * Uses NTSC 640x480 interlaced field mode with a 32-bit RGBA
 * framebuffer in GS local memory.
 */

#ifndef KOSLOAD_PS2_VIDEO_H
#define KOSLOAD_PS2_VIDEO_H

#include <stdint.h>

/* Screen dimensions */
#define PS2_SCREEN_WIDTH    640
#define PS2_SCREEN_HEIGHT   480

/* Character cell size (12x24 font, matches GC) */
#define PS2_CHAR_WIDTH      12
#define PS2_CHAR_HEIGHT     24

/* Initialize the GS for NTSC 640x480i output */
void ps2_video_init(void);

/* Clear the framebuffer with a color (0x00RRGGBB format) */
void ps2_video_clear(uint32_t color);

/* Draw a string at pixel position (x, y) with the given color */
void ps2_video_draw_string(int x, int y, const char *str, uint32_t color);

/* Fill a rectangle at (x, y) with dimensions (w, h) in the given color */
void ps2_video_fill_rect(int x, int y, int w, int h, uint32_t color);

/* Draw a 1-bit bitmap at (x, y) with dimensions (w, h).
 * bits is packed MSB-first, 32 bits per word per row.
 * Foreground pixels use color, background is transparent. */
void ps2_video_draw_bitmap(int x, int y, int w, int h,
                           const uint32_t *bits, uint32_t color);

/* Functions expected by crt0.S exception handler header and common code */
void setup_video(uint32_t mode, uint32_t bg_color);
void clear_screen(uint32_t color);
void draw_string(int x, int y, const char *str, uint32_t color);
void clear_lines(int y, int height, unsigned int color);
void uint_to_string(unsigned int val, unsigned char *buf);
const char *exception_code_to_string(uint32_t code);

#endif /* KOSLOAD_PS2_VIDEO_H */
