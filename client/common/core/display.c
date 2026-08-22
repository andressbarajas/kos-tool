/* client/common/core/display.c
 *
 * The console-side video contract, implemented once.
 *
 * setup_video / clear_screen / draw_string / clear_lines / uint_to_string are
 * the flat names the per-console entry headers (client/<console>/crt0.S,
 * client/xbox/kosload_header.c) export to guest programs, and that shared code
 * calls directly rather than through target_ops.  Each forwards to the port's
 * target_ops callback; uint_to_string is pure formatting.
 *
 * The ops table is a static const per port, so these are safe to call from any
 * context, exception handlers included -- nothing here needs initialising.
 */

#include <kosload/display.h>
#include <kosload/target.h>

/* Ports that predate target_ops::screen_width leave it 0 and are all 640 wide. */
#define DEFAULT_SCREEN_WIDTH 640

/* Weak: the entry header names these functions, so any link that pulls in
 * crt0.o pulls in this file too -- including the PS2's outer bootstrap ELF,
 * which has no target_ops and would fail on a strong reference.  Where the
 * symbol is absent the forwards below no-op. */
extern const target_ops_t *target_get_ops(void) __attribute__((weak));

void setup_video(unsigned int mode, unsigned int color) {
    const target_ops_t *t = target_get_ops ? target_get_ops() : 0;

    if(t && t->setup_video)
        t->setup_video(mode, color);
}

void clear_screen(unsigned int color) {
    const target_ops_t *t = target_get_ops ? target_get_ops() : 0;

    if(t && t->clear_screen)
        t->clear_screen(color);
}

void draw_string(int x, int y, const char *str, unsigned int color) {
    const target_ops_t *t = target_get_ops ? target_get_ops() : 0;

    if(t && t->draw_string)
        t->draw_string(x, y, str, color);
}

void clear_lines(int y, int height, unsigned int color) {
    const target_ops_t *t = target_get_ops ? target_get_ops() : 0;
    unsigned int width;

    if(!t || !t->fill_rect)
        return;

    width = t->screen_width ? t->screen_width : DEFAULT_SCREEN_WIDTH;
    t->fill_rect(0, y, (int)width, height, color);
}

/* Zero-padded 8-digit hex.
 *
 * The width is a contract, not a preference: callers pass [9] buffers, and
 * serial_transport.c's progress line positions its separators by assuming
 * exactly 8 characters.  Neither tolerates a variable-length field. */
void uint_to_string(unsigned int value, unsigned char *buf) {
    static const char hex[] = "0123456789ABCDEF";
    int i;

    for(i = 7; i >= 0; i--) {
        buf[i] = (unsigned char)hex[value & 0xF];
        value >>= 4;
    }
    buf[8] = '\0';
}
