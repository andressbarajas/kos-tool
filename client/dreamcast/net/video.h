/* client/dreamcast/net/video.h */
/*
 * Compatibility header - provides video function declarations
 * for the DC network stack files.
 *
 * Based on dcload-ip: dcload-ip/target-src/dcload/video.h
 */
#ifndef __VIDEO_H__
#define __VIDEO_H__

/* SH-ELF compiler prepends _ to C symbols, so these match
 * the _draw_string, _clrscr, etc. labels in video.S. */
extern void draw_string(int x, int y, const char *str, int color);
extern void clrscr(int color);
extern void init_video(int cabletype, int pixelmode);
extern int  check_cable(void);
extern void *get_font_address(void);

#endif /* __VIDEO_H__ */
