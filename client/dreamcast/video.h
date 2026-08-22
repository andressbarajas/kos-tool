/* client/dreamcast/video.h */
/*
 * Dreamcast video output.
 *
 * The drawing primitives are SH-4 assembly in video.S, rendering with the
 * Dreamcast BIOS ROM font; this header is their only declaration.  video.c
 * adds the bring-up sequence (cable detect -> mode set -> clear) on top.
 *
 * Colours here are 16-bit RGB565 written straight into VRAM at 0xa5000000,
 * not the 0x00RRGGBB the other ports' *_video_* entry points take;
 * dreamcast_target_ops masks the shared code's colours down to 16 bits.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/video.h
 */
#ifndef KOSLOAD_DC_VIDEO_H
#define KOSLOAD_DC_VIDEO_H

#include <stdint.h>

/* setup_video / clear_screen / draw_string / clear_lines / uint_to_string are
 * the shared video contract */
#include <kosload/display.h>

/* From video.S.  SH-ELF prepends _ to C symbols, so these match the
 * _dc_video_draw_string, _dc_video_clear, etc. labels there. */
void dc_video_draw_string(int x, int y, const char *str, int color);
void dc_video_clear(int color);
void dc_video_init(int cabletype, int pixelmode);
int  dc_video_check_cable(void);
void *get_font_address(void);

/* From video.c: the real bring-up behind target_ops::setup_video. */
void dc_video_setup(uint32_t mode, uint32_t bg_color);

#endif /* KOSLOAD_DC_VIDEO_H */
