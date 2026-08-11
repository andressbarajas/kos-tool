/* client/psp/video.h — PSP framebuffer text console. */
#ifndef KOSLOAD_PSP_VIDEO_H
#define KOSLOAD_PSP_VIDEO_H

#include <stdint.h>

/* PSP LCD: 480x272, framebuffer stride 512 pixels, 32bpp (RGBA8888). */
#define PSP_SCREEN_WIDTH   480
#define PSP_SCREEN_HEIGHT  272
#define PSP_FB_STRIDE      512
#define PSP_CHAR_WIDTH     12
#define PSP_CHAR_HEIGHT    24

void psp_video_init(void);
/* Attach drawing helpers to the framebuffer already bound by stage 1 without
 * resetting LCDC/DMACPlus or issuing a Syscon command.  Diagnostic use only. */
void psp_video_attach_existing(void);
void psp_video_clear(uint32_t color);
void psp_video_draw_string(int x, int y, const char *str, uint32_t color);
void psp_video_fill_rect(int x, int y, int w, int h, uint32_t color);
void psp_video_draw_bitmap(int x, int y, int w, int h, const uint32_t *bits, uint32_t color);

/* Names expected by the kosload header and shared common code. */
void setup_video(uint32_t mode, uint32_t bg_color);
void clear_screen(uint32_t color);
void draw_string(int x, int y, const char *str, uint32_t color);
void clear_lines(int y, int height, unsigned int color);
void uint_to_string(uint32_t val, unsigned char *buf);
const char *exception_code_to_string(uint32_t code);

#endif /* KOSLOAD_PSP_VIDEO_H */
