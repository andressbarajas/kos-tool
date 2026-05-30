/* client/playstation2/target.c */
/*
 * PlayStation 2 target_ops implementation.
 *
 * Connects the target interface to PS2-specific hardware functions:
 * GS video, cache management, and exception handling.
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <kosload/target.h>
#include <kosload/protocol.h>

#include "video.h"
#include "cache.h"
#include "ee_cop0.h"
#include "ee_sif.h"
#include "iop_smap.h"
#include "sif_broker.h"

/* From go.S */
extern void go(unsigned int addr);

/* From exception.S */
extern void exception_common(void);

static void ps2_quiesce_cop0_timer(void) {
    uint32_t status = ee_cop0_read_status();

    /* The loader polls COP0 Count for timekeeping; it never needs the COP0
     * timer interrupt.  A firmware update can inherit Cause.IP7 from the old
     * EE context, so acknowledge Compare and keep the timer mask down. */
    ee_cop0_write_status(status & ~(EE_COP0_STATUS_IE | EE_COP0_STATUS_INT5_TIMER));
    ee_cop0_write_compare(0xffffffff);
}

/* ===== Exception vector installation ===== */

/*
 * MIPS R5900 exception vectors (KSEG0 mapped, BEV=0):
 *   0x80000000 - TLB refill
 *   0x80000080 - performance counter
 *   0x80000100 - debug
 *   0x80000180 - general exception
 *   0x80000200 - interrupt
 *
 * Each stub loads the address of exception_common into k0 and jumps to it.
 * Stub layout (4 instructions, 16 bytes):
 *   lui  k0, %hi(exception_common)
 *   ori  k0, k0, %lo(exception_common)
 *   jr   k0
 *   nop
 */
static void install_exception_stub(uint32_t vector_addr) {
    volatile uint32_t *dst =
        (volatile uint32_t *)((vector_addr & 0x1fffffff) | 0xa0000000);
    uint32_t handler = (uint32_t)exception_common;
    uint16_t hi = (handler >> 16) & 0xFFFF;
    uint16_t lo = handler & 0xFFFF;

    /* lui k0, %hi(handler) — k0 = $26 = reg 26 */
    dst[0] = 0x3C1A0000 | hi; /* lui $k0, hi */
    /* ori k0, k0, %lo(handler) */
    dst[1] = 0x375A0000 | lo; /* ori $k0, $k0, lo */
    /* jr k0 */
    dst[2] = 0x03400008; /* jr $k0 */
    /* nop (branch delay slot) */
    dst[3] = 0x00000000;

    /* KSEG1 stores bypass D-cache.  The firmware-update trampoline already
     * index-invalidates the whole I-cache before entering this loader, so do
     * not issue hit-based cache ops against the vector area here. */
    (void)vector_addr;
    __asm__ volatile("sync.l\nsync.p" ::: "memory");
}

/*
 * C exception handler called from exception.S.
 *
 * For PS2 bring-up, transport is not yet usable when most exceptions
 * fire — the host can't see anything we'd write to it.  Instead we
 * draw the COP0 state to screen so the user can see what fired.
 *
 * Save area offsets (see exception.S): GPRs r0..r31 each 8 bytes
 * starting at 0x000; COP0 EPC at 0x100, Status 0x104, Cause 0x108,
 * BadVAddr 0x10C.  We read GPR low halves at +0 within each 8-byte
 * slot (little-endian).
 */
extern void progexit(int status);

/* Save area size from exception.S: 416 bytes */
#define EXC_SAVE_AREA_SIZE    416

static volatile uint32_t exception_depth;
static char exception_first_line[] =
    "EX C=00000000 E=00 EPC=00000000 BAD=00000000";
static char exception_second_line[] =
    "SP=00000000 RA=00000000 GP=00000000";

static void hex_byte(char *dst, uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";

    dst[0] = hex[(value >> 4) & 0x0f];
    dst[1] = hex[value & 0x0f];
}

static void hex_word(char *dst, uint32_t value) {
    hex_byte(dst + 0, value >> 24);
    hex_byte(dst + 2, value >> 16);
    hex_byte(dst + 4, value >> 8);
    hex_byte(dst + 6, value);
}

void exception_handler_c(uint32_t cause, uint32_t epc, uint32_t *save_area) {
    uint32_t excode = (cause >> 2) & 0x1F;
    uint32_t bvaddr = ee_cop0_read_badvaddr();
    uint32_t sp;
    uint32_t ra;
    uint32_t gp;

    if(exception_depth++ != 0) {
        /* Nested fault inside the handler — stop here rather than recurse. */
        for (;;) {}
    }

    hex_word(exception_first_line + 5, cause);
    hex_byte(exception_first_line + 16, excode);
    hex_word(exception_first_line + 23, epc);
    hex_word(exception_first_line + 36, bvaddr);

    ps2_video_draw_string(10, 30, exception_first_line, 0xffffff);

    /* GPRs from save area: each is sd'd at offset reg*8.  The low
     * 32 bits live at index (reg*8)/4 = reg*2 on a little-endian
     * 32-bit-indexed view of the save area. */
    sp = save_area[29 * 2];
    ra = save_area[31 * 2];
    gp = save_area[28 * 2];

    hex_word(exception_second_line + 3, sp);
    hex_word(exception_second_line + 15, ra);
    hex_word(exception_second_line + 27, gp);
    ps2_video_draw_string(10, 60, exception_second_line, 0xffffff);

    /* Halt forever so the user can read the screen.  Don't call
     * progexit() — it's a stub.  Don't return — exception.S would
     * jump back to _start and erase what we just drew. */
    for (;;) {}
}

/* ===== Exception initialization ===== */

void exception_init(void) {
    ps2_quiesce_cop0_timer();

    /* Install exception stubs at all standard EE vectors */
    install_exception_stub(EE_COP0_VEC_TLB_REFILL);
    install_exception_stub(EE_COP0_VEC_PERF_COUNTER);
    install_exception_stub(EE_COP0_VEC_DEBUG);
    install_exception_stub(EE_COP0_VEC_COMMON);
    install_exception_stub(EE_COP0_VEC_INTERRUPT);
}

/* ===== target_ops implementations ===== */

static int ps2_init(void) {
    ps2_quiesce_cop0_timer();
    ps2_video_init();
    ps2_sif_broker_publish();
    return 0;
}

static void ps2_draw_string(int x, int y, const char *str, uint32_t color) {
    ps2_video_draw_string(x, y, str, color);
}

static void ps2_clear_screen(uint32_t color) {
    ps2_video_clear(color);
}

static void ps2_setup_video(uint32_t mode, uint32_t bg_color) {
    (void)mode;
    (void)bg_color;
    /* Video already initialized in ps2_init() */
}

static void ps2_execute(uint32_t address) {
    (void)ps2_smap_release_pending();

    /* Flush caches for the loaded program region */
    cache_flush_range((const void *)address, 0x01000000);
    go(address);
}

static void ps2_disable_cache(void) {
    cache_disable();
}

static void ps2_reboot(void) {
    /* Jump back to kosload entry point */
    cache_disable();
    void (*entry)(void) = (void (*)(void))PS2_LOADER_BASE;
    entry();
}

/* Global wrapper for commands.c which calls disable_cache() by name */
void disable_cache(void) {
    cache_disable();
}

static void ps2_set_console_enabled(bool enabled) {
    /* Toggle ps2loadmagic (entry+0x08) between 0xdeadbeef and 0xfeedface */
    if(enabled)
        *(volatile unsigned int *)(PS2_LOADER_BASE + 8) = 0xdeadbeef;
    else
        *(volatile unsigned int *)(PS2_LOADER_BASE + 8) = 0xfeedface;
}

/* PS2 RTC access is mediated by CDVDMAN on the IOP.  The SMAP IRX owns
 * that import and exposes a small RPC so the EE-side target hooks can
 * behave like the DC/GC real RTC hooks.
 *
 * Time basis: both set_rtc and get_rtc use **Unix UTC seconds** here.
 * The IRX converts to/from the mechacon JST calendar at the hardware
 * boundary (see ps2_smap_get_rtc / ps2_smap_set_rtc).  The fallback
 * value used when the IRX RPC is unreachable is the last UTC seconds
 * the EE supplied via set_rtc. */
static uint32_t fallback_rtc = 0;

static void ps2_set_rtc(uint32_t unix_timestamp) {
    fallback_rtc = unix_timestamp;
    (void)ps2_smap_set_rtc(unix_timestamp);
}

static uint32_t ps2_get_rtc(void) {
    uint32_t rtc;

    if(ps2_smap_get_rtc(&rtc) == 0)
        return rtc;
    return fallback_rtc;
}

/* ===== Timer/tick support ===== */

/* Expose a low-rate monotonic clock to common code.  The raw EE COP0 Count
 * wraps in roughly 14 seconds at the observed PS2 rate, which is too fast for
 * shared code that multiplies seconds by ticks_per_second in 32-bit math.
 * Scaling by a power of two keeps this cheap and avoids libgcc 64-bit divide.
 */
#define PS2_COP0_TICKS_PER_SEC 294912000
#define PS2_TICK_SHIFT         8
#define PS2_TICKS_PER_SEC      (PS2_COP0_TICKS_PER_SEC >> PS2_TICK_SHIFT)

/* Track high word for 64-bit monotonic counter from 32-bit COP0 Count */
static uint32_t last_count = 0;
static uint32_t count_hi = 0;

static uint64_t ps2_get_ticks(void) {
    uint32_t count;

    count = ee_cop0_read_count();
    if(count < last_count)
        count_hi++;
    last_count = count;

    return ((uint64_t)count_hi << (32 - PS2_TICK_SHIFT)) | (count >> PS2_TICK_SHIFT);
}

static void ps2_restart_timer(void) {
    /* Match the Dreamcast lifecycle: after an uploaded program returns, start
     * the loader's monotonic timer from a known point again. */
    ps2_quiesce_cop0_timer();
    ee_cop0_write_count(0);
    ee_cop0_write_compare(0xffffffff);
    last_count = 0;
    count_hi = 0;
}

/* ===== Screensaver support ===== */

static void ps2_fill_rect(int x, int y, int w, int h, uint32_t color) {
    ps2_video_fill_rect(x, y, w, h, color);
}

static void ps2_draw_bitmap(int x, int y, int w, int h, const uint32_t *bits, uint32_t color) {
    ps2_video_draw_bitmap(x, y, w, h, bits, color);
}

/* ===== Memory detection ===== */

static uint32_t ps2_detect_ram_size(void) {
    /* All PS2 consoles have 32 MB EE RAM */
    return 32 * 1024 * 1024;
}

/* ===== Target operations structure ===== */

const target_ops_t playstation2_target_ops = {
    .name = "PlayStation 2",
    .default_load = PS2_DEFAULT_LOAD_ADDR,
    .init = ps2_init,
    .draw_string = ps2_draw_string,
    .clear_screen = ps2_clear_screen,
    .setup_video = ps2_setup_video,
    .execute = ps2_execute,
    .disable_cache = ps2_disable_cache,
    .reboot = ps2_reboot,
    .cdfs_redir_save = NULL,
    .cdfs_redir_enable = NULL,
    .cdfs_redir_disable = NULL,
    .set_console_enabled = ps2_set_console_enabled,
    .set_rtc = ps2_set_rtc,
    .get_rtc = ps2_get_rtc,
    .get_ticks = ps2_get_ticks,
    .ticks_per_second = PS2_TICKS_PER_SEC,
    .fill_rect = ps2_fill_rect,
    .draw_bitmap = ps2_draw_bitmap,
    .restart_timer = ps2_restart_timer,
    .detect_ram_size = ps2_detect_ram_size,
};

const target_ops_t *target_get_ops(void) {
    return &playstation2_target_ops;
}
