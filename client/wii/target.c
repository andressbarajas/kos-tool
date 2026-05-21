/* client/wii/target.c - Wii target_ops implementation.
 *
 * This initial Wii port reuses the repo's freestanding PPC loader model and a
 * minimal VI text console.  Networking is isolated behind ios_net.c so the IOS
 * socket ABI can be filled in clean-room without contaminating common code.
 */

#include <stdint.h>
#include <string.h>
#include <kosload/target.h>
#include <kosload/protocol.h>

#include "../gamecube/video.h"
#include "../gamecube/cache.h"
#include "ios_net.h"

extern void go(unsigned int addr);
extern void exception_common(void);

static volatile bool console_enabled = false;

#define PPC_MTSPRG0_R0  0x7C1043A6
#define PPC_LIS_R0(hi)  (0x3C000000 | ((hi) & 0xFFFF))
#define PPC_ORI_R0(lo)  (0x60000000 | ((lo) & 0xFFFF))
#define PPC_MTCTR_R0    0x7C0903A6
#define PPC_LI_R0(val)  (0x38000000 | ((val) & 0xFFFF))
#define PPC_BCTR        0x4E800420

static void install_exception_stub(uint32_t vector)
{
    volatile uint32_t *dst = (volatile uint32_t *)(0x80000000 + vector);
    uint32_t handler = (uint32_t)exception_common;

    dst[0] = PPC_MTSPRG0_R0;
    dst[1] = PPC_LIS_R0(handler >> 16);
    dst[2] = PPC_ORI_R0(handler);
    dst[3] = PPC_MTCTR_R0;
    dst[4] = PPC_LI_R0(vector);
    dst[5] = PPC_BCTR;

    cache_flush_range((const void *)(0x80000000 + vector), 24);
}

extern int write(int fd, const void *buf, unsigned int count);
extern void progexit(int status);

void exception_handler_c(uint32_t type, uint32_t srr0, uint32_t *save_area)
{
    uint8_t buf[432];
    (void)type;
    (void)srr0;

    buf[0] = 'E'; buf[1] = 'X'; buf[2] = 'P'; buf[3] = 'T';
    memcpy(buf + 4, save_area, 428);
    write(1, buf, 432);
    progexit(0);
}

void exception_init(void)
{
    static const uint32_t vectors[] = {
        0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700,
        0x0800, 0x0900, 0x0C00, 0x0D00, 0x0F00, 0x1300, 0x1700,
    };
    unsigned int i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
        install_exception_stub(vectors[i]);
}

static int wii_init(void)
{
    gc_video_init();
    return 0;
}

static void wii_draw_string(int x, int y, const char *str, uint32_t color)
{
    gc_video_draw_string(x, y, str, color);
}

static void wii_clear_screen(uint32_t color)
{
    gc_video_clear(color);
}

static void wii_setup_video(uint32_t mode, uint32_t bg_color)
{
    (void)mode;
    (void)bg_color;
}

static void wii_execute(uint32_t address)
{
    cache_flush_range((const void *)address, 0x01800000);
    go(address);
}

/* Firmware-update handoffs are one-way: the trampoline never returns through
 * cmd_execute, so the bb->stop / init / start cycle there is unreachable and
 * the old loader's IOS UDP socket would stay bound to NET_DEFAULT_PORT.  The
 * replacement loader's bind() would then return -3 (in use).  Tearing IOS
 * down here closes the socket cleanly before the handoff. */
static void wii_fw_update_prepare(void)
{
    wii_ios_net_shutdown();
}

static void wii_disable_cache(void)
{
    cache_disable();
}

static void wii_reboot(void)
{
    cache_disable();
    void (*entry)(void) = (void (*)(void))WII_LOADER_BASE;
    entry();
}

void disable_cache(void)
{
    cache_disable();
}

static void wii_set_console_enabled(bool enabled)
{
    console_enabled = enabled;
    *(volatile unsigned int *)(WII_LOADER_BASE + 4) =
        enabled ? 0xdeadbeef : 0xfeedface;
}

static void wii_set_rtc(uint32_t unix_timestamp)
{
    (void)unix_timestamp;
}

static uint32_t wii_get_rtc(void)
{
    return 0;
}

#define WII_TBR_TICKS_PER_SEC 60750000u

static uint64_t wii_get_ticks(void)
{
    uint32_t tbl, tbu0, tbu1;

    do {
        __asm__ volatile("mftbu %0" : "=r"(tbu0));
        __asm__ volatile("mftb  %0" : "=r"(tbl));
        __asm__ volatile("mftbu %0" : "=r"(tbu1));
    } while (tbu0 != tbu1);

    return ((uint64_t)tbu0 << 32) | tbl;
}

static void wii_restart_timer(void)
{
}

#define XFB_ADDR 0xC0050000

static void wii_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    uint32_t fill = gc_color_to_yuy2(color);
    int row;

    for (row = 0; row < h; row++) {
        uint32_t *fb32 = (uint32_t *)(XFB_ADDR +
                          (y + row) * GC_SCREEN_WIDTH * 2 + (x & ~1) * 2);
        int pairs = (w + 1) / 2;
        int i;
        for (i = 0; i < pairs; i++)
            fb32[i] = fill;
    }

    cache_flush_dc((const void *)(XFB_ADDR + y * GC_SCREEN_WIDTH * 2),
                   h * GC_SCREEN_WIDTH * 2);
}

static void wii_draw_bitmap(int x, int y, int w, int h,
                            const uint32_t *bits, uint32_t color)
{
    uint32_t yuy2 = gc_color_to_yuy2(color);
    uint8_t c_y = (yuy2 >> 24) & 0xFF;
    uint8_t c_cb = (yuy2 >> 16) & 0xFF;
    uint8_t c_cr = yuy2 & 0xFF;
    int words_per_row = (w + 31) / 32;
    int row, col;

    for (row = 0; row < h; row++) {
        const uint32_t *row_bits = bits + row * words_per_row;
        for (col = 0; col < w; col += 2) {
            int word0 = col / 32;
            int bit0 = 31 - (col % 32);
            int word1 = (col + 1) / 32;
            int bit1 = 31 - ((col + 1) % 32);
            int p0 = (row_bits[word0] >> bit0) & 1;
            int p1 = (col + 1 < w) ? ((row_bits[word1] >> bit1) & 1) : 0;
            uint32_t *fb32;

            if (!p0 && !p1)
                continue;

            fb32 = (uint32_t *)(XFB_ADDR + (y + row) * GC_SCREEN_WIDTH * 2 +
                                ((x + col) & ~1) * 2);
            if (p0 && p1) {
                *fb32 = ((uint32_t)c_y << 24) | ((uint32_t)c_cb << 16) |
                        ((uint32_t)c_y << 8) | (uint32_t)c_cr;
            } else {
                uint32_t existing = *fb32;
                uint8_t y0 = p0 ? c_y : (existing >> 24) & 0xFF;
                uint8_t y1 = p1 ? c_y : (existing >> 8) & 0xFF;
                *fb32 = ((uint32_t)y0 << 24) | ((uint32_t)c_cb << 16) |
                        ((uint32_t)y1 << 8) | (uint32_t)c_cr;
            }
        }
    }

    cache_flush_dc((const void *)(XFB_ADDR + y * GC_SCREEN_WIDTH * 2),
                   h * GC_SCREEN_WIDTH * 2);
}

static uint32_t wii_detect_ram_size(void)
{
    return 88 * 1024 * 1024;
}

const target_ops_t wii_target_ops = {
    .name = "Wii",
    .default_load = WII_DEFAULT_LOAD_ADDR,
    .init = wii_init,
    .draw_string = wii_draw_string,
    .clear_screen = wii_clear_screen,
    .setup_video = wii_setup_video,
    .execute = wii_execute,
    .disable_cache = wii_disable_cache,
    .reboot = wii_reboot,
    .set_console_enabled = wii_set_console_enabled,
    .set_rtc = wii_set_rtc,
    .get_rtc = wii_get_rtc,
    .get_ticks = wii_get_ticks,
    .ticks_per_second = WII_TBR_TICKS_PER_SEC,
    .fill_rect = wii_fill_rect,
    .draw_bitmap = wii_draw_bitmap,
    .restart_timer = wii_restart_timer,
    .detect_ram_size = wii_detect_ram_size,
    .fw_update_prepare = wii_fw_update_prepare,
};

const target_ops_t *target_get_ops(void)
{
    return &wii_target_ops;
}
