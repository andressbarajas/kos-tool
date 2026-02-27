/* client/common/screensaver.c */
/*
 * Platform-independent screensaver for kosload.
 *
 * Bouncing 32x32 bitmap icon on a black screen, activates after
 * 30 seconds of idle time. Uses target_ops_t for all hardware
 * access: get_ticks/ticks_per_second for timing, fill_rect for
 * erasing, draw_bitmap for rendering, clear_screen for blanking.
 */

#include <kosload/screensaver.h>
#include <kosload/target.h>
#include <kosload/divutil.h>

extern const target_ops_t *common_get_target(void);

/* Screensaver parameters */
#define IDLE_TIMEOUT_SECS  30
#define ICON_SIZE          32
#define SCREEN_W           640
#define SCREEN_H           480
#define TARGET_FPS         60

#define BLACK_COLOR        0

/* 32x32 1bpp icon (apple with K), MSB = leftmost pixel */
static const uint32_t screensaver_icon[32] = {
    0x00000000, 0x01F00000, 0x03FC0600, 0x01FE0F00,
    0x01FF1F00, 0x00FF9E00, 0x007FB800, 0x001FB000,
    0x0007E000, 0x03F9CFC0, 0x07FFFFE0, 0x0FFFFFF0,
    0x1F87E1F8, 0x1F87C1F8, 0x3F8783FC, 0x3F8707FC,
    0x3F860FFC, 0x3F841FFC, 0x3F803FFC, 0x3F807FFC,
    0x3F803FFC, 0x3F841FFC, 0x1F860FFC, 0x1F8707FC,
    0x0F8783F8, 0x0F87C1F8, 0x0787E0F0, 0x0387F0E0,
    0x01FFFFC0, 0x00FFFF80, 0x007FFF00, 0x003E3E00,
};

/* State */
static void (*restore_callback)(void);
static uint64_t timer_start;
static uint64_t last_frame;
static bool active;
static unsigned int icon_color_saved;
static int box_x, box_y;
static int box_dx, box_dy;
static unsigned int frame_interval;

void screensaver_init(void (*restore_cb)(void), unsigned int icon_color)
{
    const target_ops_t *t = common_get_target();

    restore_callback = restore_cb;
    icon_color_saved = icon_color;
    active = false;
    timer_start = t->get_ticks();
    frame_interval = UDIV_CONST(t->ticks_per_second, TARGET_FPS);
}

bool screensaver_is_active(void)
{
    return active;
}

void screensaver_reset(void)
{
    const target_ops_t *t = common_get_target();

    active = false;
    timer_start = t->get_ticks();
}

bool screensaver_wake(void)
{
    const target_ops_t *t = common_get_target();

    if (!active)
        return false;

    active = false;
    timer_start = t->get_ticks();

    if (restore_callback)
        restore_callback();

    return true;
}

void screensaver_poll(void)
{
    const target_ops_t *t = common_get_target();
    uint64_t now = t->get_ticks();

    if (!active) {
        /* Check if idle timeout has elapsed (unsigned subtraction handles wrap) */
        uint64_t elapsed = now - timer_start;
        unsigned int timeout_ticks = IDLE_TIMEOUT_SECS * t->ticks_per_second;
        if (elapsed < timeout_ticks)
            return;

        /* Activate screensaver */
        active = true;
        t->clear_screen(BLACK_COLOR);
        box_x = 100;
        box_y = 100;
        box_dx = 2;
        box_dy = 2;
        last_frame = now;
        t->draw_bitmap(box_x, box_y, ICON_SIZE, ICON_SIZE,
                       screensaver_icon, 0xffff);
        return;
    }

    /* Time-based frame pacing: skip until one frame interval has elapsed */
    if (now - last_frame < frame_interval)
        return;
    last_frame = now;

    /* Erase old position */
    t->fill_rect(box_x, box_y, ICON_SIZE, ICON_SIZE, BLACK_COLOR);

    /* Update position */
    box_x += box_dx;
    box_y += box_dy;

    /* Bounce off edges */
    if (box_x <= 0 || box_x + ICON_SIZE >= SCREEN_W) {
        box_dx = -box_dx;
        box_x += box_dx;
    }
    if (box_y <= 0 || box_y + ICON_SIZE >= SCREEN_H) {
        box_dy = -box_dy;
        box_y += box_dy;
    }

    /* Draw icon at new position */
    t->draw_bitmap(box_x, box_y, ICON_SIZE, ICON_SIZE,
                   screensaver_icon, icon_color_saved);
}
