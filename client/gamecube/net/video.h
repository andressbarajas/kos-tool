/* client/gamecube/net/video.h */
/*
 * Video function compatibility header for GameCube.
 * Maps DC video function names to GC equivalents.
 */

#ifndef __VIDEO_H__
#define __VIDEO_H__

#include "../video.h"

/* DC video functions → GC video functions */
#define draw_string     gc_video_draw_string
#define _draw_string    gc_video_draw_string
#define _clrscr         gc_video_clear
#define clear_lines(y, h, c)  /* no-op: GC overwrites text in place */

#define STR_COLOR 0x00FFFFFF

#endif /* __VIDEO_H__ */
