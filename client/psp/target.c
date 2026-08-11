/* client/psp/target.c — PSP target_ops implementation.
 *
 * Connects the shared client interface to PSP hardware: the VRAM framebuffer
 * console (video.c), Allegrex cache maintenance (cache.h), and the COP0 cycle
 * counter as a monotonic clock.
 */

#include <stdint.h>
#include <stdbool.h>
#include <kosload/target.h>
#include <kosload/protocol.h>
#include <kosload/types.h>

#include "video.h"
#include "cache.h"
#include "syscon.h"
#include "usb/usb_dev.h"

/* The linker script reserves the top PSP_LZO_WRKMEM_SIZE bytes of the loader
 * window for the host-assigned LZO work memory, but it cannot see protocol.h.
 * Tie the two constants together here so they cannot drift apart silently --
 * a mismatch would either under-reserve (LZO hash tables landing in the
 * loader's BSS) or over-reserve and fail the link for no reason. */
_Static_assert(PSP_LZO_WRKMEM_SIZE == LZO_WRKMEM_SIZE,
               "PSP_LZO_WRKMEM_SIZE (mk/memory.mk) != LZO_WRKMEM_SIZE (protocol.h)");
_Static_assert(PSP_LZO_WRKMEM_ADDR == PSP_LOADER_BASE + PSP_LOADER_SIZE - LZO_WRKMEM_SIZE,
               "PSP LZO work memory is not at the top of the loader window");

/* From go.S */
extern void go(uint32_t addr);

/* Host-side crash reporting, shared with exception.c.  write() delivers the
 * staged "EXPT" frame to kos-tool's handle_psp_exception and progexit() tells
 * it the program is over, exactly as a normally-exiting program would. */
extern int  write(int fd, const void *buf, unsigned int count);
extern void progexit(int status);

extern uint8_t           psp_exc_wire[];
extern volatile uint32_t psp_exc_pending;

/* Non-zero only while a loaded program is executing.  The crash handler needs
 * this to tell a resumable guest fault from a fault in loader code, where go()
 * has no saved context to return through. */
volatile uint32_t psp_guest_running;

/* Allegrex COP0 Count rate, MEASURED on PSP-1000 rather than assumed.
 *
 * This was 222000000 (the PSP's default game-mode CPU clock) on the theory that
 * Count ticks once per CPU cycle.  It does, but the loader does not run at
 * 222 MHz: sampling the loader's own screensaver timer over three consecutive
 * ~13 s intervals through the host gave 331.37, 331.44 and 331.78 MHz, i.e. the
 * 333 MHz clock.  The old value was 1.5x low, which is directly visible: the
 * screensaver animated at ~90 FPS instead of 60 and armed after ~20 s instead
 * of 30, and every "one second" wait built on this constant was ~0.67 s.
 *
 * Still approximate — it is used only for the screensaver, coarse timeouts and
 * the ~1 Hz status display, none of which need better than a percent. */
#define PSP_TICKS_PER_SEC 333000000u

static bool console_enabled = true;

static uint32_t read_count(void) {
    uint32_t c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

/* 64-bit monotonic clock from the 32-bit COP0 Count. */
static uint32_t last_count = 0;
static uint32_t count_hi = 0;

static int psp_init(void) {
    psp_video_init();
    return 0;
}

static void psp_draw_string(int x, int y, const char *str, uint32_t color) {
    psp_video_draw_string(x, y, str, color);
}

static void psp_clear_screen(uint32_t color) {
    psp_video_clear(color);
}

static void psp_setup_video(uint32_t mode, uint32_t bg_color) {
    (void)mode;
    (void)bg_color;
}

static void psp_execute(uint32_t address) {
    cache_flush_range((const void *)address, 0x01000000);

    psp_guest_running = 1;
    go(address);
    psp_guest_running = 0;

    /* go() returns here both when a program exits normally and when it faults
     * -- exception.c resumes through go_return() after staging a crash frame.
     * Ship the frame now rather than from the handler: back in normal context
     * the USB pipe has a valid stack and its poll loop again, neither of which
     * an exception context can promise. */
    if(psp_exc_pending) {
        psp_exc_pending = 0;
        write(1, psp_exc_wire, sizeof(psp_exception_frame_t));
        progexit(0);
    }
}

static void psp_disable_cache(void) {
    cache_disable();
}

void disable_cache(void) {
    cache_disable();
}

static void psp_reboot(void) {
    /* Bare-metal reboot, no firmware call: assert the "Top" (main system) reset
     * in the sysreg reset-enable register.  0xBC10004C bit0 = Top (per psdevwiki
     * + Sony's sceSysreg reset accessor in lowio.prx).  The firmware's own cold
     * reset uses a syscon RESET_DEVICE command instead, but the sysreg poke is
     * the simplest pure-hardware path. */
    *(volatile uint32_t *)0xBC10004Cu |= 1u;
    for(;;)
        __asm__ volatile("");
}

/* Called instead of the console setup when the EXEC is a firmware-update
 * trampoline rather than a user program.  The jump is one-way -- the trampoline
 * copies the replacement loader over this one and enters it -- so anything the
 * USB controller still has armed against our buffers has to be retired before
 * that memory is overwritten. */
static void psp_fw_update_prepare(void) {
    usb_dev_prepare_handoff();
}

static void psp_set_console_enabled(bool enabled) {
    console_enabled = enabled;
}

static uint64_t psp_get_ticks(void) {
    uint32_t c = read_count();
    if(c < last_count)
        count_hi++;
    last_count = c;
    return ((uint64_t)count_hi << 32) | c;
}

/* The PSP keeps its wall clock in the syscon microcontroller, reachable only
 * through a syscon command whose byte and response layout we have not RE'd --
 * and psp_syscon_cmd() is send-only today, so reading one is not just a matter
 * of knowing the number.  Until that is done the host-supplied timestamp is all
 * we have, so anchor it to the COP0 cycle counter and let it run: a program
 * that samples the clock twice a minute apart sees a minute pass, instead of
 * the same frozen second the previous constant-return version reported.  It
 * does NOT survive a power cycle; the host sets it again on the next run. */
static uint32_t rtc_epoch;       /* Unix seconds at the anchor, 0 = never set */
static uint64_t rtc_epoch_ticks; /* psp_get_ticks() when the anchor was taken */

static void psp_set_rtc(uint32_t unix_timestamp) {
    rtc_epoch = unix_timestamp;
    rtc_epoch_ticks = psp_get_ticks();
}

static uint32_t psp_get_rtc(void) {
    if(rtc_epoch == 0)
        return 0; /* never set — report "unknown" rather than 1970 plus uptime */

    return rtc_epoch + (uint32_t)((psp_get_ticks() - rtc_epoch_ticks) / PSP_TICKS_PER_SEC);
}

static void psp_restart_timer(void) {
    /* Zeroing Count moves the clock's reference out from under it, so fold the
     * elapsed time into the anchor first, or a returning program would rewind
     * the RTC to whatever the host last set.  Only commands.c calls this, so
     * the serial path the PSP uses never reaches it today -- this keeps the
     * clock correct if it ever does. */
    rtc_epoch = psp_get_rtc();
    rtc_epoch_ticks = 0;

    __asm__ volatile("mtc0 $zero, $9");
    last_count = 0;
    count_hi = 0;
}

static void psp_fill_rect(int x, int y, int w, int h, uint32_t color) {
    psp_video_fill_rect(x, y, w, h, color);
}

static void psp_draw_bitmap(int x, int y, int w, int h, const uint32_t *bits, uint32_t color) {
    psp_video_draw_bitmap(x, y, w, h, bits, color);
}

static uint32_t psp_detect_ram_size(void) {
    /* Build-time policy, not a speculative size probe.  Stage 1 opens
     * BC000000..BC00000C and exception_init() re-verifies the writes before
     * transport startup, so the full 32 MiB is reportable; the loader sits at
     * the bottom of it, leaving the guest one unbroken extent (see the map in
     * mk/memory.mk).  A failure there is fatal, never a silent downgrade. */
    return (uint32_t)PSP_RAM_BYTES;
}

const target_ops_t psp_target_ops = {
    .name = "PSP",
    .default_load = PSP_DEFAULT_LOAD_ADDR,
    .init = psp_init,
    .draw_string = psp_draw_string,
    .clear_screen = psp_clear_screen,
    .setup_video = psp_setup_video,
    .execute = psp_execute,
    .disable_cache = psp_disable_cache,
    .reboot = psp_reboot,
    .cdfs_redir_save = NULL,
    .cdfs_redir_enable = NULL,
    .cdfs_redir_disable = NULL,
    .set_console_enabled = psp_set_console_enabled,
    .set_rtc = psp_set_rtc,
    .get_rtc = psp_get_rtc,
    .get_ticks = psp_get_ticks,
    .ticks_per_second = PSP_TICKS_PER_SEC,
    .fill_rect = psp_fill_rect,
    .draw_bitmap = psp_draw_bitmap,
    .restart_timer = psp_restart_timer,
    .detect_ram_size = psp_detect_ram_size,
    .fw_update_prepare = psp_fw_update_prepare,
    .screen_width = PSP_SCREEN_WIDTH,   /* 480 */
    .screen_height = PSP_SCREEN_HEIGHT, /* 272 */
};

const target_ops_t *target_get_ops(void) {
    return &psp_target_ops;
}

/* exception_init() lives in exception.c. */
