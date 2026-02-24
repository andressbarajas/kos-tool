/* client/include/kosload/video.h */
#ifndef KOSLOAD_VIDEO_H
#define KOSLOAD_VIDEO_H

#include <stdint.h>

/* Video display interface for client status output */
void video_init(uint32_t mode, uint32_t bg_color);
void video_clear(uint32_t color);
void video_draw_string(int x, int y, const char *str, uint32_t color);
void video_clear_lines(int y, int height, uint32_t color);

/* Common video constants */
#define VIDEO_COLOR_WHITE   0xFFFFFFFF
#define VIDEO_COLOR_BLACK   0x00000000
#define VIDEO_COLOR_GREEN   0xFF00FF00
#define VIDEO_COLOR_RED     0xFFFF0000

#endif /* KOSLOAD_VIDEO_H */
