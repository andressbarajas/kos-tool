/* client/gamecube/target.c */
/*
 * GameCube target_ops implementation.
 *
 * Connects the target interface to GC-specific hardware functions:
 * video, EXI/USBGecko, cache management, and exception handling.
 */

#include <stdint.h>
#include <kosload/target.h>
#include <kosload/protocol.h>

#include "video.h"
#include "cache.h"
#include "exi.h"
#include "serial/usbgecko.h"

/* From go.S */
extern void go(unsigned int addr);

/* From exception.S */
extern void exception_common(void);

/* Console enabled flag */
static volatile bool console_enabled = false;

/* ===== Exception vector installation ===== */

/* PPC machine instruction encodings for exception stubs */
#define PPC_MTSPRG0_R0  0x7C1043A6  /* mtsprg 0, r0 */
#define PPC_LIS_R0(hi)  (0x3C000000 | ((hi) & 0xFFFF))
#define PPC_ORI_R0(lo)  (0x60000000 | ((lo) & 0xFFFF))
#define PPC_MTCTR_R0    0x7C0903A6  /* mtctr r0 */
#define PPC_LI_R0(val)  (0x38000000 | ((val) & 0xFFFF))
#define PPC_BCTR        0x4E800420  /* bctr */

static void install_exception_stub(uint32_t vector)
{
    volatile uint32_t *dst = (volatile uint32_t *)(0x80000000 + vector);
    uint32_t handler = (uint32_t)exception_common;

    /* Write stub instructions:
     *   mtsprg 0, r0               ; save r0
     *   lis r0, handler@h          ; load handler address high
     *   ori r0, r0, handler@l      ; load handler address low
     *   mtctr r0                   ; move to CTR
     *   li r0, vector_id           ; load exception type
     *   bctr                       ; jump to handler
     */
    dst[0] = PPC_MTSPRG0_R0;
    dst[1] = PPC_LIS_R0(handler >> 16);
    dst[2] = PPC_ORI_R0(handler);
    dst[3] = PPC_MTCTR_R0;
    dst[4] = PPC_LI_R0(vector);
    dst[5] = PPC_BCTR;

    /* Flush D-cache and invalidate I-cache for the stub */
    cache_flush_range((const void *)(0x80000000 + vector), 24);
}

/*
 * C exception handler called from exception.S.
 * Displays exception type and PC on screen, then returns to
 * the asm handler which reboots the loader.
 */
void exception_handler_c(uint32_t type, uint32_t srr0, uint32_t *save_area)
{
    unsigned char type_str[9], pc_str[9];

    (void)save_area;

    gc_video_clear(0);
    gc_video_draw_string(0, 0, gc_exception_code_to_string(type), 0x00FFFFFF);

    gc_video_draw_string(0, 24, "Type:", 0x00FFFFFF);
    uint_to_string(type, type_str);
    gc_video_draw_string(72, 24, (const char *)type_str, 0x00FFFFFF);

    gc_video_draw_string(0, 48, "PC:  ", 0x00FFFFFF);
    uint_to_string(srr0, pc_str);
    gc_video_draw_string(72, 48, (const char *)pc_str, 0x00FFFFFF);
}

/* ===== Exception initialization ===== */

void exception_init(void)
{
    /* Install exception stubs at all standard PPC 750 vectors */
    static const uint32_t vectors[] = {
        0x0100, /* System Reset */
        0x0200, /* Machine Check */
        0x0300, /* DSI */
        0x0400, /* ISI */
        0x0500, /* External Interrupt */
        0x0600, /* Alignment */
        0x0700, /* Program */
        0x0800, /* FP Unavailable */
        0x0900, /* Decrementer */
        0x0C00, /* System Call */
        0x0D00, /* Trace */
        0x0F00, /* Performance Monitor */
        0x1300, /* IABR */
        0x1700, /* Thermal */
    };
    unsigned int i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
        install_exception_stub(vectors[i]);
}

/* ===== target_ops implementations ===== */

static int gc_init(void)
{
    exi_init();
    gc_video_init();
    return 0;
}

static void gc_draw_string(int x, int y, const char *str, uint32_t color)
{
    gc_video_draw_string(x, y, str, color);
}

static void gc_clear_screen(uint32_t color)
{
    gc_video_clear(color);
}

static void gc_setup_video(uint32_t mode, uint32_t bg_color)
{
    (void)mode;
    (void)bg_color;
    /* Video already initialized in gc_init() */
}

static void gc_execute(uint32_t address)
{
    /* Flush caches for the loaded program region */
    cache_flush_range((const void *)address, 0x01000000);
    go(address);
}

static void gc_disable_cache(void)
{
    cache_disable();
}

static void gc_reboot(void)
{
    /* Jump back to kosload entry point (GC_LOADER_BASE from mk/memory.mk) */
    cache_disable();
    void (*entry)(void) = (void (*)(void))GC_LOADER_BASE;
    entry();
}

static const char *gc_exception_to_string(uint32_t code)
{
    return gc_exception_code_to_string(code);
}

static void gc_cdfs_redir_save(void)
{
    /* No optical drive redirection on GameCube */
}

static void gc_cdfs_redir_enable(void)
{
    /* No-op on GameCube */
}

/* Global wrappers for commands.c which calls these by name (not via vtable).
 * On DC these are provided by disable.S and cdfs_redir.S. */
void disable_cache(void)
{
    cache_disable();
}

void cdfs_redir_enable(void)
{
    /* No-op on GameCube */
}

static void gc_cdfs_redir_disable(void)
{
    /* No-op on GameCube */
}

static void gc_set_console_enabled(bool enabled)
{
    console_enabled = enabled;
    /* Write magic value at loader base+4 for loaded program to detect */
    if (enabled)
        *(volatile unsigned int *)(GC_LOADER_BASE + 4) = 0xdeadbeef;
    else
        *(volatile unsigned int *)(GC_LOADER_BASE + 4) = 0xfeedface;
}

/* GC RTC via EXI channel 0, device 1 (Macronix MX23L4005).
 * 32-bit seconds counter, epoch 2000-01-01.
 *
 * The GC IPL displays: rtc_counter + counter_bias (SRAM offset 0x0C).
 * counter_bias encodes timezone and user adjustments set in IPL setup.
 * To set the displayed time correctly, we must account for this bias:
 *   new_rtc = desired_time - counter_bias
 * so that new_rtc + counter_bias = desired_time. */
#define UNIX_TO_GC_OFFSET 946684800u  /* seconds from 1970-01-01 to 2000-01-01 */

/* SRAM read command: 0x20000100 + (byte_offset << 6) */
#define SRAM_COUNTER_BIAS_OFFSET  0x0C

static uint32_t gc_get_rtc(void)
{
    /* Read raw RTC counter (seconds since 2000-01-01).
     * We skip counter_bias (SRAM 0x0C) here — it's only needed for
     * human-readable display (IPL).  For elapsed-time calculations
     * (pre/post program exec) the bias cancels out, and reading SRAM
     * after a loaded program returns can give inconsistent results. */
    exi_select(0, 1, 3);
    exi_imm(0, 0x20000000, 4, EXI_IMM_WRITE);
    uint32_t rtc_val = exi_imm(0, 0, 4, EXI_IMM_READ);
    exi_deselect(0);

    return rtc_val + UNIX_TO_GC_OFFSET;
}

static void gc_set_rtc(uint32_t unix_timestamp)
{
    uint32_t desired_gc_time = unix_timestamp - UNIX_TO_GC_OFFSET;

    /* Read counter_bias from SRAM (offset 0x0C) */
    exi_select(0, 1, 3);
    exi_imm(0, 0x20000100 + (SRAM_COUNTER_BIAS_OFFSET << 6), 4, EXI_IMM_WRITE);
    uint32_t counter_bias = exi_imm(0, 0, 4, EXI_IMM_READ);
    exi_deselect(0);

    /* Subtract bias so IPL displays the correct time */
    uint32_t new_rtc = desired_gc_time - counter_bias;

    /* Write new RTC counter */
    exi_select(0, 1, 3);
    exi_imm(0, 0xA0000000, 4, EXI_IMM_WRITE);
    exi_imm(0, new_rtc, 4, EXI_IMM_WRITE);
    exi_deselect(0);
}

/* ===== Screensaver support: timer, fill_rect, input polling ===== */

/* GC Time Base Register: 40.5 MHz */
#define GC_TBR_TICKS_PER_SEC  40500000

static uint64_t gc_get_ticks(void)
{
    uint32_t tbl, tbu0, tbu1;
    /* Read TBR with rollover protection */
    do {
        __asm__ volatile("mftbu %0" : "=r"(tbu0));
        __asm__ volatile("mftb  %0" : "=r"(tbl));
        __asm__ volatile("mftbu %0" : "=r"(tbu1));
    } while (tbu0 != tbu1);
    return ((uint64_t)tbu0 << 32) | tbl;
}

/* XFB address and dimensions (must match video.c) */
#define XFB_ADDR  0xC0050000

static void gc_restart_timer(void)
{
    /* TBR is always running on GameCube; nothing to restart */
}

static void gc_fill_rect(int x, int y, int w, int h, uint32_t color)
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

    /* Flush modified framebuffer region */
    cache_flush_dc((const void *)(XFB_ADDR + y * GC_SCREEN_WIDTH * 2),
                   h * GC_SCREEN_WIDTH * 2);
}

static void gc_draw_bitmap(int x, int y, int w, int h,
                           const uint32_t *bits, uint32_t color)
{
    uint32_t yuy2 = gc_color_to_yuy2(color);
    uint8_t c_y  = (yuy2 >> 24) & 0xFF;
    uint8_t c_cb = (yuy2 >> 16) & 0xFF;
    uint8_t c_cr = yuy2 & 0xFF;
    int words_per_row = (w + 31) / 32;
    int row, col;

    for (row = 0; row < h; row++) {
        const uint32_t *row_bits = bits + row * words_per_row;
        /* Process 2 pixels at a time (YUY2 constraint) */
        for (col = 0; col < w; col += 2) {
            int word0 = col / 32;
            int bit0 = 31 - (col % 32);
            int word1 = (col + 1) / 32;
            int bit1 = 31 - ((col + 1) % 32);
            int p0 = (row_bits[word0] >> bit0) & 1;
            int p1 = (col + 1 < w) ? ((row_bits[word1] >> bit1) & 1) : 0;

            /* Skip pixel pairs where neither bit is set */
            if (!p0 && !p1)
                continue;

            uint32_t *fb32 = (uint32_t *)(XFB_ADDR +
                              (y + row) * GC_SCREEN_WIDTH * 2 +
                              ((x + col) & ~1) * 2);

            if (p0 && p1) {
                /* Both pixels foreground */
                *fb32 = ((uint32_t)c_y << 24) | ((uint32_t)c_cb << 16) |
                        ((uint32_t)c_y << 8) | (uint32_t)c_cr;
            } else {
                /* One pixel set, one not — read existing pair, replace set pixel */
                uint32_t existing = *fb32;
                uint8_t y0 = p0 ? c_y : (existing >> 24) & 0xFF;
                uint8_t y1 = p1 ? c_y : (existing >> 8) & 0xFF;
                /* Use foreground chroma when at least one pixel is set */
                *fb32 = ((uint32_t)y0 << 24) | ((uint32_t)c_cb << 16) |
                        ((uint32_t)y1 << 8) | (uint32_t)c_cr;
            }
        }
    }

    cache_flush_dc((const void *)(XFB_ADDR + y * GC_SCREEN_WIDTH * 2),
                   h * GC_SCREEN_WIDTH * 2);
}

static uint32_t gc_detect_ram_size(void)
{
    /* GameCube has 24MB main RAM (MEM1), no hardware variation */
    return 24 * 1024 * 1024;
}

const target_ops_t gamecube_target_ops = {
    .name = "GameCube",
    .default_load = GC_DEFAULT_LOAD_ADDR,
    .init = gc_init,
    .draw_string = gc_draw_string,
    .clear_screen = gc_clear_screen,
    .setup_video = gc_setup_video,
    .execute = gc_execute,
    .disable_cache = gc_disable_cache,
    .reboot = gc_reboot,
    .exception_to_string = gc_exception_to_string,
    .cdfs_redir_save = gc_cdfs_redir_save,
    .cdfs_redir_enable = gc_cdfs_redir_enable,
    .cdfs_redir_disable = gc_cdfs_redir_disable,
    .set_console_enabled = gc_set_console_enabled,
    .set_rtc = gc_set_rtc,
    .get_rtc = gc_get_rtc,
    .get_ticks = gc_get_ticks,
    .ticks_per_second = GC_TBR_TICKS_PER_SEC,
    .fill_rect = gc_fill_rect,
    .draw_bitmap = gc_draw_bitmap,
    .restart_timer = gc_restart_timer,
    .detect_ram_size = gc_detect_ram_size,
};

const target_ops_t *target_get_ops(void)
{
    return &gamecube_target_ops;
}
