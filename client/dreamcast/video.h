/* client/dreamcast/video.h */
/*
 * Dreamcast video output.
 *
 * The drawing primitives are SH-4 assembly in video.S, rendering with the
 * Dreamcast BIOS ROM font; this header is their only declaration.
 *
 * Colours are 16-bit RGB565 written straight into VRAM at 0xa5000000.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/video.h
 */
#ifndef KOSLOAD_DC_VIDEO_H
#define KOSLOAD_DC_VIDEO_H

/* From video.S.  SH-ELF prepends _ to C symbols, so these match the
 * _draw_string, _dc_video_clear, etc. labels there. */
void draw_string(int x, int y, const char *str, int color);
void dc_video_clear(int color);
void dc_video_init(int cabletype, int pixelmode);
int  dc_video_check_cable(void);
void *get_font_address(void);

#endif /* KOSLOAD_DC_VIDEO_H */
