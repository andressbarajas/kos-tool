/* client/xbox/video.h — Xbox dumb-framebuffer text console.
 *
 * Renders status text into the framebuffer the Xbox kernel already set up,
 * using the shared 12x24 bitmap font.  Colors are passed in 0x00RRGGBB form
 * (matching the other ports' target_ops draw_string contract); the framebuffer
 * itself is XRGB8888.
 */

#ifndef KOSLOAD_XBOX_VIDEO_H
#define KOSLOAD_XBOX_VIDEO_H

#include <stdint.h>

/* setup_video / clear_screen / draw_string / clear_lines / uint_to_string are
 * the shared video contract */
#include <kosload/display.h>

/* The retail kernel/dashboard leaves an active framebuffer; video.c reads its
 * base, stride and height back from the live mode (PCRTC/CRTC/PRAMDAC), so
 * XBOX_SCREEN_HEIGHT — and XBOX_SCREEN_WIDTH as the stride — apply only if that
 * readback fails.  XBOX_SCREEN_WIDTH is also the unconditional horizontal clip
 * for drawing, so text stays in the leftmost 640 pixels of a wider mode. */
#define XBOX_SCREEN_WIDTH   640
#define XBOX_SCREEN_HEIGHT  480
#define XBOX_CHAR_WIDTH     12
#define XBOX_CHAR_HEIGHT    24

/* Front-panel LED boot breadcrumb (SMBus SMC; independent of NV2A video).
 * Pattern nibbles: high = red phases, low = green phases. */
#define XBOX_LED_OFF     0x00u
#define XBOX_LED_GREEN   0x0Fu
#define XBOX_LED_RED     0xF0u
#define XBOX_LED_ORANGE  0xFFu
void xbox_smc_led(uint8_t pattern);
/* Hardware reset via the chipset reset-control register (port 0xCF9) — reboots
 * to the dashboard; reliable even after the loader owns the IDT/GDT. */
void xbox_reset(void);

void xbox_video_init(void);
/* Idempotent bring-up: initialise only if no framebuffer is bound yet.  This is
 * what xbox_target_ops::setup_video calls. */
void xbox_video_ensure(void);
/* Re-pin NV2A scanout, re-halt the pushbuffer, and re-un-blank; call each
 * main-loop iteration so the display survives the kernel's post-handoff
 * boot-animation teardown. */
void xbox_video_kick(void);
void xbox_video_clear(uint32_t color);
void xbox_video_draw_string(int x, int y, const char *str, uint32_t color);
void xbox_video_fill_rect(int x, int y, int w, int h, uint32_t color);
void xbox_video_draw_bitmap(int x, int y, int w, int h, const uint32_t *bits, uint32_t color);

#endif /* KOSLOAD_XBOX_VIDEO_H */
